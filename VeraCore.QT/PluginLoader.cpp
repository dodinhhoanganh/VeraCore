#include "PluginLoader.h"

#include <QDir>
#include <QDirIterator>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSet>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>

#include <windows.h>

// Real SDK headers (3rdparty)
#include <vst.h>                                    // VST2 (clean-room SDK)
#include <pluginterfaces/base/ipluginbase.h>        // VST3 factory interfaces
#include <pluginterfaces/vst/ivstaudioprocessor.h>  // kVstAudioEffectClass
#include <clap/entry.h>
#include <clap/factory/plugin-factory.h>

namespace {

//==============================================================================
// Raw scan results. Plain PODs only: these cross the SEH boundary, so nothing
// in here may require C++ unwinding.
//==============================================================================

constexpr int kMaxRawPlugins = 64;
constexpr qsizetype kMaxHelperOutputBytes = 1024 * 1024;
constexpr qsizetype kMaxHelperStderrBytes = 64 * 1024;
constexpr char kScanProtocolPrefix[] = "VERACORE_SCAN_RESULT_V1:";

QJsonObject PluginInfoToJson(const PluginInfo& info)
{
    QJsonObject object;
    object["path"] = info.path;
    object["name"] = info.name;
    object["type"] = info.type;
    object["vendor"] = info.vendor;
    object["version"] = info.version;
    object["category"] = info.category;
    object["uid"] = info.uid;
    return object;
}

bool PluginInfoFromJson(const QJsonValue& value, PluginInfo& info)
{
    if (!value.isObject()) return false;
    const QJsonObject object = value.toObject();
    info.path = object.value("path").toString();
    info.name = object.value("name").toString().left(1024);
    info.type = object.value("type").toString().left(256);
    info.vendor = object.value("vendor").toString().left(1024);
    info.version = object.value("version").toString().left(256);
    info.category = object.value("category").toString().left(256);
    info.uid = object.value("uid").toString().left(1024);
    return !info.path.isEmpty() && !info.type.isEmpty();
}

QByteArray EncodePluginInfo(const PluginInfo& info)
{
    return QJsonDocument(PluginInfoToJson(info)).toJson(QJsonDocument::Compact)
        .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

bool DecodePluginInfo(const QByteArray& encoded, PluginInfo& info)
{
    if (encoded.isEmpty() || encoded.size() > kMaxHelperOutputBytes) return false;
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(
        QByteArray::fromBase64(encoded, QByteArray::Base64UrlEncoding), &parseError);
    return parseError.error == QJsonParseError::NoError && document.isObject() &&
        PluginInfoFromJson(document.object(), info);
}

QByteArray EncodeScanResults(const std::vector<PluginInfo>& results)
{
    QJsonArray array;
    for (const PluginInfo& info : results) array.append(PluginInfoToJson(info));
    return QJsonDocument(array).toJson(QJsonDocument::Compact)
        .toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
}

bool DecodeScanResults(const QByteArray& encoded, const PluginInfo& candidate,
                       std::vector<PluginInfo>& results)
{
    if (encoded.isEmpty() || encoded.size() > kMaxHelperOutputBytes) return false;
    QJsonParseError parseError{};
    const QJsonDocument document = QJsonDocument::fromJson(
        QByteArray::fromBase64(encoded, QByteArray::Base64UrlEncoding), &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isArray() ||
        document.array().size() > kMaxRawPlugins) return false;
    for (const QJsonValue& value : document.array()) {
        PluginInfo info;
        if (!PluginInfoFromJson(value, info) ||
            QFileInfo(info.path).absoluteFilePath().compare(
                QFileInfo(candidate.path).absoluteFilePath(), Qt::CaseInsensitive) != 0 ||
            (!info.type.startsWith("Invalid") &&
             info.type.compare(candidate.type, Qt::CaseInsensitive) != 0))
            return false;
        results.push_back(std::move(info));
    }
    return !results.empty();
}

void AppendBoundedTail(QByteArray& destination, const QByteArray& chunk, qsizetype limit)
{
    if (chunk.isEmpty() || limit <= 0) return;
    destination.append(chunk);
    if (destination.size() > limit)
        destination.remove(0, destination.size() - limit);
}

std::vector<PluginInfo> ScannerFailure(const PluginInfo& candidate, const QString& reason,
                                       QString* errorText)
{
    if (errorText) *errorText = reason;
    PluginInfo invalid = candidate;
    invalid.type = QString("Invalid (Scanner %1)").arg(reason.left(160));
    return {std::move(invalid)};
}

enum ScanError {
    kScanOk = 0,
    kScanLoadFailed = 1,     // LoadLibrary failed (win32Error holds the code)
    kScanNoEntryPoint = 2,   // module loaded but exports are missing
    kScanCrashed = 3,        // SEH exception while probing
    kScanInitFailed = 4,     // entry/factory refused to initialize
    kScanNoPlugins = 5       // module is valid but exposes no audio plugins
};

struct RawPluginData {
    char name[256];
    char vendor[256];
    char version[64];
    char uid[64];
    bool isInstrument;
};

struct RawScanResult {
    RawPluginData plugins[kMaxRawPlugins];
    int count;
    int error;
    unsigned long win32Error;
};

enum ModuleKind { kKindVst2 = 0, kKindVst3 = 1, kKindClap = 2 };

void CopyCStr(char* dst, size_t cap, const char* src)
{
    if (!dst || cap == 0)
        return;
    size_t i = 0;
    if (src)
        for (; i + 1 < cap && src[i]; ++i)
            dst[i] = src[i];
    dst[i] = 0;
}

//==============================================================================
// VST2
//==============================================================================

typedef vst_effect_t* (VST_FUNCTION_INTERFACE* Vst2EntryProc)(vst_host_callback_t);

intptr_t VST_FUNCTION_INTERFACE Vst2HostCallback(vst_effect_t* /*effect*/, int32_t opcode,
                                                 int32_t /*p_int1*/, int64_t /*p_int2*/,
                                                 const char* p_str, float /*p_float*/)
{
    switch (opcode) {
    case VST_HOST_OPCODE_VST_VERSION:        // audioMasterVersion
        return 2400;
    case VST_HOST_OPCODE_CURRENT_EFFECT_ID:  // audioMasterCurrentId
        return 0;
    case 0x10:                               // audioMasterGetSampleRate
        return 44100;
    case 0x11:                               // audioMasterGetBlockSize
        return 512;
    case 0x17:                               // audioMasterGetCurrentProcessLevel
        return 0;
    case VST_HOST_OPCODE_VENDOR_NAME:
        CopyCStr(const_cast<char*>(p_str), VST_BUFFER_SIZE_VENDOR_NAME, "VeraCore");
        return 1;
    case VST_HOST_OPCODE_PRODUCT_NAME:
        CopyCStr(const_cast<char*>(p_str), VST_BUFFER_SIZE_PRODUCT_NAME, "VeraCore");
        return 1;
    case 0x22:                               // audioMasterGetVendorVersion
        return 1000;
    default:
        return 0;
    }
}

void ScanVst2Impl(HMODULE hModule, RawScanResult* out)
{
    Vst2EntryProc entry = (Vst2EntryProc)GetProcAddress(hModule, "VSTPluginMain");
    if (!entry)
        entry = (Vst2EntryProc)GetProcAddress(hModule, "main");
    if (!entry) {
        out->error = kScanNoEntryPoint;
        return;
    }

    vst_effect_t* effect = entry(&Vst2HostCallback);
    if (!effect || effect->magic_number != VST_MAGICNUMBER) {
        out->error = kScanInitFailed;
        return;
    }

    effect->control(effect, VST_EFFECT_OPCODE_CREATE, 0, 0, nullptr, 0.0f); // effOpen

    // Some plugins return 0 from the name opcodes yet still fill the buffer,
    // so the buffer content is the only reliable signal.
    RawPluginData* p = &out->plugins[0];
    char buffer[256] = {};
    effect->control(effect, VST_EFFECT_OPCODE_EFFECT_NAME, 0, 0, buffer, 0.0f);
    if (buffer[0])
        CopyCStr(p->name, sizeof(p->name), buffer);

    buffer[0] = 0;
    effect->control(effect, VST_EFFECT_OPCODE_VENDOR_NAME, 0, 0, buffer, 0.0f);
    if (buffer[0])
        CopyCStr(p->vendor, sizeof(p->vendor), buffer);

    p->isInstrument = (effect->flags & VST_EFFECT_FLAG_INSTRUMENT) != 0;

    char numBuf[32];
    sprintf_s(numBuf, "%d", effect->version);
    CopyCStr(p->version, sizeof(p->version), numBuf);
    sprintf_s(numBuf, "%08X", (unsigned)effect->unique_id);
    CopyCStr(p->uid, sizeof(p->uid), numBuf);

    effect->control(effect, VST_EFFECT_OPCODE_DESTROY, 0, 0, nullptr, 0.0f); // effClose

    out->count = 1;
    out->error = kScanOk;
}

//==============================================================================
// VST3
//==============================================================================

typedef Steinberg::IPluginFactory* (PLUGIN_API* GetFactoryProc)();
typedef bool (PLUGIN_API* InitExitDllProc)();

void Vst3UidToHex(const Steinberg::TUID cid, char* dst, size_t cap)
{
    static const char* hex = "0123456789ABCDEF";
    size_t pos = 0;
    for (int i = 0; i < 16 && pos + 2 < cap; ++i) {
        unsigned char b = (unsigned char)cid[i];
        dst[pos++] = hex[b >> 4];
        dst[pos++] = hex[b & 0x0F];
    }
    dst[pos] = 0;
}

void ScanVst3Impl(HMODULE hModule, RawScanResult* out)
{
    using namespace Steinberg;

    GetFactoryProc getFactory = (GetFactoryProc)GetProcAddress(hModule, "GetPluginFactory");
    if (!getFactory) {
        out->error = kScanNoEntryPoint;
        return;
    }

    // Many plugins (reFX Nexus included) require InitDll before the factory works.
    InitExitDllProc initDll = (InitExitDllProc)GetProcAddress(hModule, "InitDll");
    InitExitDllProc exitDll = (InitExitDllProc)GetProcAddress(hModule, "ExitDll");
    bool inited = false;
    if (initDll) {
        if (!initDll()) {
            out->error = kScanInitFailed;
            return;
        }
        inited = true;
    }

    IPluginFactory* factory = getFactory();
    if (!factory) {
        if (inited && exitDll)
            exitDll();
        out->error = kScanInitFailed;
        return;
    }

    PFactoryInfo factoryInfo;
    bool hasFactoryInfo = (factory->getFactoryInfo(&factoryInfo) == kResultOk);

    IPluginFactory2* factory2 = nullptr;
    factory->queryInterface(IPluginFactory2_iid, (void**)&factory2);

    int32 classCount = factory->countClasses();
    for (int32 i = 0; i < classCount && out->count < kMaxRawPlugins; ++i) {
        if (factory2) {
            PClassInfo2 ci2;
            if (factory2->getClassInfo2(i, &ci2) != kResultOk)
                continue;
            if (strcmp(ci2.category, kVstAudioEffectClass) != 0)
                continue;
            RawPluginData* p = &out->plugins[out->count++];
            CopyCStr(p->name, sizeof(p->name), ci2.name);
            CopyCStr(p->vendor, sizeof(p->vendor),
                     ci2.vendor[0] ? ci2.vendor : (hasFactoryInfo ? factoryInfo.vendor : ""));
            CopyCStr(p->version, sizeof(p->version), ci2.version);
            p->isInstrument = (strstr(ci2.subCategories, "Instrument") != nullptr);
            Vst3UidToHex(ci2.cid, p->uid, sizeof(p->uid));
        } else {
            PClassInfo ci;
            if (factory->getClassInfo(i, &ci) != kResultOk)
                continue;
            if (strcmp(ci.category, kVstAudioEffectClass) != 0)
                continue;
            RawPluginData* p = &out->plugins[out->count++];
            CopyCStr(p->name, sizeof(p->name), ci.name);
            CopyCStr(p->vendor, sizeof(p->vendor), hasFactoryInfo ? factoryInfo.vendor : "");
            p->isInstrument = false;
            Vst3UidToHex(ci.cid, p->uid, sizeof(p->uid));
        }
    }

    if (factory2)
        factory2->release();
    factory->release();
    if (inited && exitDll)
        exitDll();

    out->error = (out->count > 0) ? kScanOk : kScanNoPlugins;
}

//==============================================================================
// CLAP
//==============================================================================

void ScanClapImpl(HMODULE hModule, const char* utf8Path, RawScanResult* out)
{
    const clap_plugin_entry_t* entry =
        (const clap_plugin_entry_t*)GetProcAddress(hModule, "clap_entry");
    if (!entry) {
        out->error = kScanNoEntryPoint;
        return;
    }
    if (!entry->init || !entry->get_factory || entry->clap_version.major < 1) {
        out->error = kScanInitFailed;
        return;
    }
    if (!entry->init(utf8Path)) {
        out->error = kScanInitFailed;
        return;
    }

    const clap_plugin_factory_t* factory =
        (const clap_plugin_factory_t*)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    if (factory) {
        uint32_t pluginCount = factory->get_plugin_count(factory);
        for (uint32_t i = 0; i < pluginCount && out->count < kMaxRawPlugins; ++i) {
            const clap_plugin_descriptor_t* desc = factory->get_plugin_descriptor(factory, i);
            if (!desc || !desc->name)
                continue;
            RawPluginData* p = &out->plugins[out->count++];
            CopyCStr(p->name, sizeof(p->name), desc->name);
            CopyCStr(p->vendor, sizeof(p->vendor), desc->vendor);
            CopyCStr(p->version, sizeof(p->version), desc->version);
            CopyCStr(p->uid, sizeof(p->uid), desc->id);
            if (desc->features)
                for (const char* const* f = desc->features; *f; ++f)
                    if (strcmp(*f, CLAP_PLUGIN_FEATURE_INSTRUMENT) == 0)
                        p->isInstrument = true;
        }
    }

    if (entry->deinit)
        entry->deinit();

    out->error = (out->count > 0) ? kScanOk : kScanNoPlugins;
}

//==============================================================================
// SEH boundary. This function must contain no objects requiring unwinding.
// Plugins are foreign code: anything from a missing dependency to a crashing
// DllMain must not take the host down.
//==============================================================================

void ScanModuleSeh(const wchar_t* widePath, const char* utf8Path, int kind,
                   RawScanResult* out) noexcept
{
    HMODULE hModule = nullptr;
    __try {
        // LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR resolves the plugin's own dependent
        // DLLs against the plugin directory (required for bundled plugins such
        // as reFX Nexus 5 whose support DLLs live next to the binary).
        hModule = LoadLibraryExW(widePath, nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
        if (!hModule)
            hModule = LoadLibraryExW(widePath, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        if (!hModule) {
            out->error = kScanLoadFailed;
            out->win32Error = GetLastError();
            return;
        }

        switch (kind) {
        case kKindVst2: ScanVst2Impl(hModule, out); break;
        case kKindVst3: ScanVst3Impl(hModule, out); break;
        case kKindClap: ScanClapImpl(hModule, utf8Path, out); break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out->error = kScanCrashed;
    }

    if (hModule) {
        __try {
            FreeLibrary(hModule);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
}

QString ScanErrorToType(const RawScanResult& raw, const QString& sdkName)
{
    switch (raw.error) {
    case kScanLoadFailed:
        if (raw.win32Error == ERROR_BAD_EXE_FORMAT)
            return "Invalid (32/64-bit Architecture Mismatch)";
        if (raw.win32Error == ERROR_MOD_NOT_FOUND)
            return "Invalid (Missing Dependency DLL)";
        return QString("Invalid (LoadLibrary failed, error %1)").arg(raw.win32Error);
    case kScanNoEntryPoint:
        return QString("Invalid (No %1 Entry Point)").arg(sdkName);
    case kScanCrashed:
        return "Invalid (Crashed During Scan)";
    case kScanInitFailed:
        return QString("Invalid (%1 Initialization Failed)").arg(sdkName);
    case kScanNoPlugins:
        return QString("Invalid (No %1 Plugins In Module)").arg(sdkName);
    default:
        return "Invalid (Unknown Error)";
    }
}

// True when the entry lives inside a .vst3/.clap bundle directory. Those
// internals (resource DLLs, support binaries) must not be scanned directly.
bool IsInsideBundle(const QFileInfo& fileInfo)
{
    QDir dir = fileInfo.dir();
    while (true) {
        const QString dirName = dir.dirName();
        if (dirName.endsWith(".vst3", Qt::CaseInsensitive) ||
            dirName.endsWith(".clap", Qt::CaseInsensitive))
            return true;
        if (!dir.cdUp())
            return false;
    }
}

QString ResolveX64BundleBinary(const QString& path, const QString& extension)
{
    const QFileInfo fileInfo(path);
    if (fileInfo.isFile())
        return fileInfo.absoluteFilePath();
    if (!fileInfo.isDir())
        return QString();

    const QDir archDir(fileInfo.absoluteFilePath() + "/Contents/x86_64-win");
    if (!archDir.exists())
        return QString();

    const QFileInfoList binaries = archDir.entryInfoList(
        QStringList{ QString("*.%1").arg(extension) }, QDir::Files);
    return binaries.isEmpty() ? QString() : binaries.first().absoluteFilePath();
}

bool IsAmd64PeBinary(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() < 256)
        return false;

    IMAGE_DOS_HEADER dosHeader{};
    if (file.read(reinterpret_cast<char*>(&dosHeader), sizeof(dosHeader)) != sizeof(dosHeader) ||
        dosHeader.e_magic != IMAGE_DOS_SIGNATURE || dosHeader.e_lfanew <= 0 ||
        static_cast<qint64>(dosHeader.e_lfanew) + 4 + sizeof(IMAGE_FILE_HEADER) + sizeof(WORD) > file.size())
        return false;

    if (!file.seek(dosHeader.e_lfanew))
        return false;

    DWORD signature = 0;
    IMAGE_FILE_HEADER fileHeader{};
    WORD optionalMagic = 0;
    if (file.read(reinterpret_cast<char*>(&signature), sizeof(signature)) != sizeof(signature) ||
        file.read(reinterpret_cast<char*>(&fileHeader), sizeof(fileHeader)) != sizeof(fileHeader) ||
        file.read(reinterpret_cast<char*>(&optionalMagic), sizeof(optionalMagic)) != sizeof(optionalMagic))
        return false;

    return signature == IMAGE_NT_SIGNATURE &&
        fileHeader.Machine == IMAGE_FILE_MACHINE_AMD64 &&
        optionalMagic == IMAGE_NT_OPTIONAL_HDR64_MAGIC;
}

QString ResolveCandidateBinary(const PluginInfo& candidate)
{
    if (candidate.type.compare("VST3", Qt::CaseInsensitive) == 0)
        return ResolveX64BundleBinary(candidate.path, "vst3");
    if (candidate.type.compare("CLAP", Qt::CaseInsensitive) == 0)
        return ResolveX64BundleBinary(candidate.path, "clap");
    if (candidate.type.compare("VST2", Qt::CaseInsensitive) == 0)
        return QFileInfo(candidate.path).absoluteFilePath();
    return QString();
}

} // namespace

//==============================================================================
// PluginLoader
//==============================================================================

QString PluginLoader::ResolveVst3Binary(const QString& path)
{
    return ResolveX64BundleBinary(path, "vst3");
}

bool PluginLoader::IsX64Candidate(const PluginInfo& candidate)
{
    const QString binaryPath = ResolveCandidateBinary(candidate);
    return !binaryPath.isEmpty() && IsAmd64PeBinary(binaryPath);
}

QString PluginLoader::CandidateFingerprint(const PluginInfo& candidate)
{
    const QString binaryPath = ResolveCandidateBinary(candidate);
    const QFileInfo binary(binaryPath);
    if (binaryPath.isEmpty() || !binary.exists() || !binary.isFile()) return QString();
    return QStringLiteral("v1|%1|%2|%3")
        .arg(candidate.type.toUpper())
        .arg(binary.size())
        .arg(binary.lastModified().toMSecsSinceEpoch());
}

std::vector<PluginInfo> PluginLoader::ScanBinary(const PluginInfo& candidate)
{
    std::vector<PluginInfo> results;

    int kind;
    QString sdkName;
    QString binaryPath = candidate.path;

    if (candidate.type == "VST3") {
        kind = kKindVst3;
        sdkName = "VST3";
        binaryPath = ResolveVst3Binary(candidate.path);
        if (binaryPath.isEmpty()) {
            PluginInfo invalid = candidate;
            invalid.type = "Invalid (No 64-bit Binary In VST3 Bundle)";
            results.push_back(invalid);
            return results;
        }
    } else if (candidate.type == "CLAP") {
        kind = kKindClap;
        sdkName = "CLAP";
        binaryPath = ResolveX64BundleBinary(candidate.path, "clap");
        if (binaryPath.isEmpty()) {
            PluginInfo invalid = candidate;
            invalid.type = "Invalid (No 64-bit Binary In CLAP Bundle)";
            results.push_back(invalid);
            return results;
        }
    } else if (candidate.type == "VST2") {
        kind = kKindVst2;
        sdkName = "VST2";
    } else {
        PluginInfo invalid = candidate;
        invalid.type = "Invalid (Unknown Plugin Type)";
        results.push_back(invalid);
        return results;
    }

    if (!IsAmd64PeBinary(binaryPath)) {
        PluginInfo invalid = candidate;
        invalid.type = "Invalid (Plugin Is Not x64 AMD64)";
        results.push_back(invalid);
        return results;
    }

    auto raw = std::make_unique<RawScanResult>();
    memset(raw.get(), 0, sizeof(RawScanResult));

    const QByteArray utf8Path = binaryPath.toUtf8();

    UINT oldErrorMode = SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX);
    ScanModuleSeh(reinterpret_cast<const wchar_t*>(binaryPath.utf16()),
                  utf8Path.constData(), kind, raw.get());
    SetErrorMode(oldErrorMode);

    if (raw->error != kScanOk) {
        PluginInfo invalid = candidate;
        invalid.type = ScanErrorToType(*raw, sdkName);
        results.push_back(invalid);
        return results;
    }

    for (int i = 0; i < raw->count; ++i) {
        const RawPluginData& p = raw->plugins[i];
        PluginInfo info = candidate;
        if (p.name[0])
            info.name = QString::fromUtf8(p.name);
        info.vendor = QString::fromUtf8(p.vendor);
        info.version = QString::fromUtf8(p.version);
        info.uid = QString::fromUtf8(p.uid);
        info.category = p.isInstrument ? "Instrument" : "Effect";
        results.push_back(info);
    }
    return results;
}

std::vector<PluginInfo> PluginLoader::ScanBinaryIsolated(
    const PluginInfo& candidate, int timeoutMs, QString* errorText)
{
    if (errorText) errorText->clear();
    if (!IsX64Candidate(candidate)) return ScanBinary(candidate);
    const QString executable = QCoreApplication::applicationFilePath();
    if (executable.isEmpty() || !QFileInfo::exists(executable))
        return ScannerFailure(candidate, "helper executable missing", errorText);

    QProcess process;
    process.setProgram(executable);
    process.setArguments({QStringLiteral("--veracore-scan-helper"),
                          QString::fromLatin1(EncodePluginInfo(candidate))});
    process.setProcessChannelMode(QProcess::SeparateChannels);
    process.start(QIODevice::ReadOnly);
    if (!process.waitForStarted(5000))
        return ScannerFailure(candidate, "failed to start", errorText);

    const int safeTimeout = std::clamp(timeoutMs, 1000, 120000);
    QElapsedTimer elapsed;
    elapsed.start();
    QByteArray standardOutput;
    QByteArray standardError;
    while (process.state() != QProcess::NotRunning && elapsed.elapsed() < safeTimeout) {
        const int remaining = safeTimeout - static_cast<int>(elapsed.elapsed());
        process.waitForReadyRead(std::clamp(remaining, 1, 100));
        AppendBoundedTail(standardOutput, process.readAllStandardOutput(), kMaxHelperOutputBytes);
        AppendBoundedTail(standardError, process.readAllStandardError(), kMaxHelperStderrBytes);
    }
    if (process.state() != QProcess::NotRunning) {
        process.kill();
        process.waitForFinished(2000);
        process.readAllStandardOutput();
        process.readAllStandardError();
        return ScannerFailure(candidate, QString("timeout after %1 ms").arg(safeTimeout), errorText);
    }
    AppendBoundedTail(standardOutput, process.readAllStandardOutput(), kMaxHelperOutputBytes);
    AppendBoundedTail(standardError, process.readAllStandardError(), kMaxHelperStderrBytes);
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        const QString detail = QString::fromUtf8(standardError).trimmed().left(120);
        return ScannerFailure(candidate,
            detail.isEmpty() ? QString("crashed/exit %1").arg(process.exitCode())
                             : QString("exit %1: %2").arg(process.exitCode()).arg(detail),
            errorText);
    }

    const QByteArray prefix(kScanProtocolPrefix);
    const qsizetype marker = standardOutput.lastIndexOf(prefix);
    if (marker < 0) return ScannerFailure(candidate, "protocol marker missing", errorText);
    QByteArray payload = standardOutput.mid(marker + prefix.size());
    const qsizetype newline = payload.indexOf('\n');
    if (newline >= 0) payload.truncate(newline);
    payload = payload.trimmed();
    std::vector<PluginInfo> results;
    if (!DecodeScanResults(payload, candidate, results))
        return ScannerFailure(candidate, "invalid protocol payload", errorText);
    return results;
}

int PluginLoader::RunScanHelper(const QStringList& args)
{
    if (args.size() != 3 || args[1] != QStringLiteral("--veracore-scan-helper")) return 64;
    PluginInfo candidate;
    if (!DecodePluginInfo(args[2].toLatin1(), candidate)) return 65;
    const std::vector<PluginInfo> results = ScanBinary(candidate);
    const QByteArray line = QByteArray(kScanProtocolPrefix) + EncodeScanResults(results) + '\n';
    if (std::fwrite(line.constData(), 1, static_cast<size_t>(line.size()), stdout) !=
        static_cast<size_t>(line.size())) return 74;
    std::fflush(stdout);
    return 0;
}

void PluginLoader::ExtractMetadata(PluginInfo& info)
{
    const std::vector<PluginInfo> results = ScanBinaryIsolated(info);
    if (!results.empty())
        info = results.front();
}

std::vector<PluginInfo> PluginLoader::CollectCandidates(const QStringList& searchPaths)
{
    std::vector<PluginInfo> candidates;
    QSet<QString> seenPaths;

    auto addCandidate = [&](const QString& absolutePath, const QString& baseName,
                            const QString& type) {
        if (seenPaths.contains(absolutePath))
            return;
        seenPaths.insert(absolutePath);
        PluginInfo info;
        info.path = absolutePath;
        info.name = baseName;
        info.type = type;
        candidates.push_back(info);
    };

    for (const QString& searchPath : searchPaths) {
        QDirIterator it(searchPath, QDir::AllEntries | QDir::NoDotAndDotDot,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            const QFileInfo fileInfo = it.fileInfo();
            const QString suffix = fileInfo.suffix().toLower();

            if (fileInfo.isDir()) {
                // Packaged VST3/CLAP bundle directories.
                if (suffix == "vst3")
                    addCandidate(fileInfo.absoluteFilePath(), fileInfo.completeBaseName(), "VST3");
                else if (suffix == "clap")
                    addCandidate(fileInfo.absoluteFilePath(), fileInfo.completeBaseName(), "CLAP");
                continue;
            }

            // Files inside a bundle belong to the bundle, not the scan.
            if (IsInsideBundle(fileInfo))
                continue;

            if (suffix == "vst3")
                addCandidate(fileInfo.absoluteFilePath(), fileInfo.completeBaseName(), "VST3");
            else if (suffix == "clap")
                addCandidate(fileInfo.absoluteFilePath(), fileInfo.completeBaseName(), "CLAP");
            else if (suffix == "dll")
                addCandidate(fileInfo.absoluteFilePath(), fileInfo.completeBaseName(), "VST2");
        }
    }
    return candidates;
}
