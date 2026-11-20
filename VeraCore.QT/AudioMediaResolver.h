#pragma once

#include <QString>

struct AudioMediaResolution
{
    QString storedPath;
    QString resolvedPath;
    bool online = false;
    bool relocated = false;
};

class AudioMediaResolver final
{
public:
    static bool isOnline(const QString& path);
    static AudioMediaResolution resolve(const QString& storedPath,
                                        const QString& projectFilePath);
};
