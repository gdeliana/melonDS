// Standalone tests of the production archive reader; fixtures are synthetic ROMs.
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <QCoreApplication>
#include "frontend/qt_sdl/ArchiveUtil.h"

using namespace melonDS;

static void Require(bool condition, const char* message)
{
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    Require(argc == 2, "expected fixture directory");
    const QDir fixtures(QString::fromLocal8Bit(argv[1]));
    const QString nds = "games/demo.nds";
    const QString gba = QString::fromUtf8("games/caf\xC3\xA9.gba");
    const QStringList originalFiles = fixtures.entryList(QDir::AllEntries | QDir::NoDotAndDotDot);
    for (const QString& zip : {QString("stored.zip"), QString("deflated.zip"), QString::fromUtf8("caf\xC3\xA9.zip")})
    {
        auto names = Archive::ListArchive(fixtures.filePath(zip));
        Require(names.value(0) == "OK" && names.contains(nds) && names.contains(gba), "list ROM entries");
        Require(!names.contains("games/"), "exclude directories");
        std::unique_ptr<u8[]> data;
        u32 size = 0;
        Require(Archive::ExtractFileFromArchive(fixtures.filePath(zip), nds, data, &size) == 262400,
                "load multi-chunk NDS ROM");
        Require(data && size == 262400, "ROM size and allocation");
        for (u32 i = 0; i < size; i++) Require(data[i] == (i & 255), "NDS bytes match");
        Require(Archive::ExtractFileFromArchive(fixtures.filePath(zip), gba, data, nullptr) == 8,
                "load Unicode GBA entry without size output");
        Require(std::memcmp(data.get(), "GBA test", 8) == 0, "GBA bytes match");
    }
    for (const auto& request : {
            std::make_pair("stored.zip", "missing.nds"),
            std::make_pair("stored.zip", "games/"),
            std::make_pair("missing.zip", "games/demo.nds"),
            std::make_pair("empty.zip", "games/demo.nds"),
            std::make_pair("invalid.zip", "games/demo.nds"),
            std::make_pair("corrupt.zip", "games/demo.nds"),
            std::make_pair("truncated.zip", "games/demo.nds"),
            std::make_pair("oversize.zip", "games/demo.nds"),
            std::make_pair("encrypted.zip", "games/demo.nds"),
            std::make_pair("empty-entry.zip", "empty.nds")})
    {
        auto data = std::make_unique<u8[]>(8);
        u32 size = 123;
        Require(Archive::ExtractFileFromArchive(fixtures.filePath(request.first), request.second, data, &size) == -1,
                request.first);
        Require(!data && size == 0, "failure clears partial outputs");
    }
    Require(Archive::ListArchive(fixtures.filePath("empty.zip")) == QVector<QString>{"OK"}, "empty ZIP listing");
    for (const char* path : {"missing.zip", "invalid.zip", "truncated.zip"})
        Require(Archive::ListArchive(fixtures.filePath(path)).value(0) == "Err", "invalid ZIP listing");
    Require(fixtures.entryList(QDir::AllEntries | QDir::NoDotAndDotDot) == originalFiles,
            "ROM loading creates no files in fixture directory");
    std::puts("PASS: stored/deflated ZIP, chunked reads, multiple ROMs, Unicode, missing entries, directories, empty/corrupt/truncated/encrypted/oversized entries, output cleanup");
}
