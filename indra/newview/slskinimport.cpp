/**
 * @file slskinimport.cpp
 * @brief Skin importer (#1599). See the header.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "slskinimport.h"
#include "fsyspath.h"

#include "llapp.h"
#include "lldate.h"
#include "lldir.h"
#include "llfloaterreg.h"
#include "llimage.h"
#include "llnotificationsutil.h"
#include "lluuid.h"
#include "llversioninfo.h"
#include "slfloaterviewerimport.h"
#include "workqueue.h"

#include <fstream>

namespace fs = std::filesystem;

namespace
{
bool sBusy = false;

constexpr const char* STAGING_PREFIX = ".import-";

std::string utf8(const fs::path& p)
{
    const auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

bool isDirectory(const fs::path& p)
{
    std::error_code ec;
    return fs::is_directory(fs::symlink_status(p, ec)) && !ec;
}

struct Job
{
    enum EKind { ZIP, FOLDER, FIRESTORM };
    EKind       kind = ZIP;
    std::string source_label;
    fs::path    src;                    // the zip, or the folder
    fs::path    skins_dir;              // FIRESTORM
    std::string skin_folder, theme_folder;
    fs::path    fallback_default;
    std::string fallback_name;          // manifest name when the source has none
    std::string source_viewer;
    bool        include_layout = false;
    // filled on the main thread before the work starts
    fs::path    staging, user_skins, our_default, lookup_file;
    std::string date, compat;
};

struct Outcome
{
    bool ok = false;
    std::string error;
    slskin::Report report;
    slskin::Manifest manifest;
    fs::path out_dir;
};

// The image check the conversion uses: the cheap header check, then a real decode with our codecs.
bool decodeCheck(const fs::path& file, std::string& why)
{
    constexpr uint64_t cap = 32ull * 1024 * 1024;
    if (!slskin::sniffImage(file, cap, why)) return false;
    std::string ext = utf8(file.extension());
    LLStringUtil::toLower(ext);
    if (ext == ".svg") return true;     // vector icons are parsed when drawn
    std::string bytes;
    if (!slskin::readRegularFile(file, cap, bytes) || bytes.empty())
    {
        why = "unreadable";
        return false;
    }
    try
    {
        LLPointer<LLImageFormatted> image = LLImageFormatted::createFromExtension(utf8(file.filename()));
        if (image.isNull())
        {
            why = "unsupported image format";
            return false;
        }
        bool header_ok = false;
        {
            LLImageDataLock lock(image);
            if (U8* data = image->allocateData(S32(bytes.size())))
            {
                memcpy(data, bytes.data(), bytes.size());
                header_ok = image->updateData();
            }
        }
        if (!header_ok)
        {
            why = "bad image header";
            return false;
        }
        LLPointer<LLImageRaw> raw = new LLImageRaw();
        if (!image->decode(raw, 0.f) || raw->getWidth() <= 0 || raw->getHeight() <= 0)
        {
            why = "does not decode";
            return false;
        }
        if (raw->getWidth() > 4096 || raw->getHeight() > 4096)
        {
            why = "image too large";
            return false;
        }
    }
    catch (const std::exception& e)
    {
        why = std::string("does not decode (") + e.what() + ")";
        return false;
    }
    return true;
}

bool loadOurSkin(const Job& job, slskin::OurSkin& ours)
{
    ours.default_dir = job.our_default;
    std::string text;
    if (slskin::readRegularFile(job.our_default / "colors.xml", 8 * 1024 * 1024, text))
    {
        for (const slskin::ColorDef& c : slskin::parseColors(text)) ours.colors.insert(c.name);
    }
    if (slskin::readRegularFile(job.our_default / "textures" / "textures.xml", 8 * 1024 * 1024, text))
    {
        ours.textures = slskin::parseTextures(text);
    }
    if (slskin::readRegularFile(job.lookup_file, 8 * 1024 * 1024, text))
    {
        ours.looked_up = slskin::parseLookupNames(text);
    }
    return !ours.colors.empty() && !ours.textures.empty();
}

// The layers to convert for a picked folder or an unpacked archive.
bool layersFor(const slskin::SourceGuess& guess, std::vector<slskin::Layer>& layers, std::string& source_viewer, std::string& error)
{
    switch (guess.kind)
    {
        case slskin::SourceKind::OurSkin:
            layers.push_back({ guess.root, false, utf8(guess.root.filename()) });
            return true;
        case slskin::SourceKind::FsSkin:
            layers = slskin::fsLayers(guess.skins_dir, guess.skin_folder, std::string());
            source_viewer = "Firestorm";
            break;
        case slskin::SourceKind::FsTheme:
            layers = slskin::fsLayers(guess.skins_dir, guess.skin_folder, guess.theme_folder);
            source_viewer = "Firestorm";
            break;
        case slskin::SourceKind::FsSkinsDir:
            error = "this is a whole skins folder. Use \"Import from Firestorm...\" and pick one skin and theme.";
            return false;
        default:
            error = "no colors.xml, textures or xui folder in it: it does not look like a skin.";
            return false;
    }
    if (layers.empty())
    {
        error = "the Firestorm skin folder could not be read.";
        return false;
    }
    return true;
}

Outcome runJob(const Job& job)
{
    Outcome o;
    try
    {
        slskin::Limits limits;
        slskin::OurSkin ours;
        if (!loadOurSkin(job, ours))
        {
            o.error = "this viewer's own skin files could not be read.";
            return o;
        }
        std::vector<slskin::Layer> layers;
        std::string source_viewer = job.source_viewer;
        const std::string* manifest_json = nullptr;
        std::string manifest_text;
        if (job.kind == Job::FIRESTORM)
        {
            layers = slskin::fsLayers(job.skins_dir, job.skin_folder, job.theme_folder, job.fallback_default);
            if (layers.empty())
            {
                o.error = "the Firestorm skin folder could not be read.";
                return o;
            }
        }
        else
        {
            fs::path root = job.src;
            if (job.kind == Job::ZIP)
            {
                root = job.staging / "raw";
                std::error_code ec;
                fs::create_directories(root, ec);
                if (!slskin::unpackZip(job.src, root, limits, o.report, o.error)) return o;
            }
            const slskin::SourceGuess guess = slskin::classifySource(root);
            if (!layersFor(guess, layers, source_viewer, o.error)) return o;
            if (guess.kind == slskin::SourceKind::OurSkin &&
                slskin::readRegularFile(guess.root / "manifest.json", 256 * 1024, manifest_text))
            {
                manifest_json = &manifest_text;
            }
        }

        o.manifest = slskin::resolveManifest(manifest_json, job.fallback_name, source_viewer);
        if (manifest_json && o.manifest.generated)
        {
            o.report.notes.push_back("manifest.json was unreadable or had no name: a new one was generated.");
        }
        if (o.manifest.date.empty()) o.manifest.date = job.date;
        if (o.manifest.compatibility.empty()) o.manifest.compatibility = job.compat;

        o.out_dir = job.staging / "out";
        std::error_code ec;
        fs::create_directories(o.out_dir, ec);
        slskin::BuildOptions options;
        options.include_layout = job.include_layout;
        options.limits = limits;
        if (!slskin::buildSkin(layers, ours, options, decodeCheck, o.manifest, o.out_dir, o.report))
        {
            o.error = "nothing in it could be imported.";
            return o;
        }
        o.ok = true;
    }
    catch (const std::exception& e)
    {
        o.ok = false;
        o.error = std::string("unexpected error: ") + e.what();
    }
    return o;
}

std::set<std::string> systemSkinFolders()
{
    std::set<std::string> out;
    std::error_code ec;
    for (fs::directory_iterator it(fsyspath(gDirUtilp->getSkinBaseDir()), ec), end; !ec && it != end; it.increment(ec))
    {
        if (it->is_directory(ec)) out.insert(utf8(it->path().filename()));
    }
    out.insert("default");
    return out;
}

void endJob(const SLSkinImport::done_cb_t& done, const std::string& folder)
{
    sBusy = false;
    if (done) done(folder);
}

void failJob(const Job& job, Outcome& o, const SLSkinImport::done_cb_t& done)
{
    o.report.source = job.source_label;
    LL_WARNS("SkinImport") << "Import from " << job.source_label << " failed: " << o.error << "\n"
                           << o.report.summary(100000) << LL_ENDL;
    slskin::removeTree(job.staging);
    LLSD args;
    args["SOURCE"] = job.source_label;
    args["REASON"] = o.error;
    args["SUMMARY"] = o.report.skipped.empty() ? std::string() : o.report.summary(6);
    LLNotificationsUtil::add("SkinImportFailed", args);
    endJob(done, std::string());
}

void install(const Job& job, Outcome o, const std::string& folder, bool replace, const SLSkinImport::done_cb_t& done)
{
    const fs::path target = job.user_skins / fsyspath(folder);
    const fs::path old = job.staging / "old";
    std::error_code ec;
    bool moved_old = false;
    if (replace && fs::exists(fs::symlink_status(target, ec)))
    {
        fs::rename(target, old, ec);
        if (ec)
        {
            o.error = "the installed skin \"" + folder + "\" could not be replaced (" + ec.message() + ").";
            failJob(job, o, done);
            return;
        }
        moved_old = true;
    }
    fs::rename(o.out_dir, target, ec);
    if (ec)
    {
        std::error_code ignore;
        if (moved_old) fs::rename(old, target, ignore);
        o.error = "the skin could not be moved into place (" + ec.message() + ").";
        failJob(job, o, done);
        return;
    }
    slskin::removeTree(job.staging);
    o.report.source = job.source_label;
    o.report.skin_name = o.manifest.name;
    o.report.folder = folder;
    LL_INFOS("SkinImport") << "Imported skin\n" << o.report.summary(100000) << LL_ENDL;
    LLNotificationsUtil::add("SkinImportSummary", LLSD().with("SUMMARY", o.report.summary(6)));
    endJob(done, folder);
}

void finish(const Job& job, Outcome o, const SLSkinImport::done_cb_t& done)
{
    if (!o.ok)
    {
        failJob(job, o, done);
        return;
    }
    std::string folder = slskin::safeFolderName(o.manifest.name);
    if (slskin::isReservedFolderName(folder, systemSkinFolders())) folder = "user_" + folder;
    std::error_code ec;
    if (!fs::exists(fs::symlink_status(job.user_skins / fsyspath(folder), ec)))
    {
        install(job, o, folder, false, done);
        return;
    }
    LLNotificationsUtil::add("SkinImportClash", LLSD().with("NAME", o.manifest.name), LLSD(),
        [job, o, folder, done](const LLSD& notification, const LLSD& response)
        {
            const S32 option = LLNotificationsUtil::getSelectedOption(notification, response);
            if (option == 0)
            {
                install(job, o, folder, true, done);
            }
            else if (option == 1)
            {
                Outcome copy = o;
                const fs::path skins = job.user_skins;
                const std::string unique = slskin::uniqueFolderName(folder, [&skins](const std::string& f)
                {
                    std::error_code e;
                    return fs::exists(fs::symlink_status(skins / fsyspath(f), e));
                });
                copy.manifest.name += " (" + unique.substr(unique.rfind('_') + 1) + ")";
                std::ofstream out(copy.out_dir / "manifest.json", std::ios::binary | std::ios::trunc);
                out << slskin::manifestJson(copy.manifest);
                out.close();
                install(job, copy, unique, false, done);
            }
            else
            {
                LL_INFOS("SkinImport") << "Import of " << job.source_label << " cancelled (name clash)" << LL_ENDL;
                slskin::removeTree(job.staging);
                endJob(done, std::string());
            }
        });
}

void start(Job job, const SLSkinImport::done_cb_t& done)
{
    if (sBusy)
    {
        LLNotificationsUtil::add("SkinImportBusy");
        return;
    }
    SLSkinImport::sweepStaging();
    sBusy = true;
    job.user_skins = fsyspath(SLSkinImport::userSkinsDir());
    job.staging = job.user_skins / (std::string(STAGING_PREFIX) + LLUUID::generateNewID().asString().substr(0, 8));
    std::error_code ec;
    fs::create_directories(job.staging, ec);
    if (ec)
    {
        Outcome o;
        o.error = "the skins folder could not be written (" + ec.message() + ").";
        failJob(job, o, done);
        return;
    }
    job.our_default = fsyspath(gDirUtilp->getSkinBaseDir()) / "default";
    job.lookup_file = fsyspath(gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "xui_lookup_names.txt"));
    job.date = LLDate::now().asString();
    job.compat = LLVersionInfo::instance().getShortVersion();
    LL_INFOS("SkinImport") << "Importing a skin from " << job.source_label << (job.include_layout ? " (with layout files)" : "")
                           << LL_ENDL;

    auto work = [job]() -> Outcome { return runJob(job); };
    auto done_main = [job, done](Outcome o)
    {
        if (LLApp::isExiting()) return;
        finish(job, o, done);
    };
    LL::WorkQueue::ptr_t main_queue = LL::WorkQueue::getInstance("mainloop");
    LL::WorkQueue::ptr_t general = LL::WorkQueue::getInstance("General");
    if (!(main_queue && general && main_queue->postTo(general, work, done_main))) done_main(work());
}
}

// static
std::string SLSkinImport::userSkinsDir()
{
    return gDirUtilp->add(gDirUtilp->getOSUserAppDir(), "skins");
}

// static
bool SLSkinImport::busy()
{
    return sBusy;
}

// static
void SLSkinImport::sweepStaging()
{
    if (sBusy) return;  // the running import's own staging folder is in there
    std::error_code ec;
    const fs::path skins = fsyspath(userSkinsDir());
    std::vector<fs::path> stale;
    for (fs::directory_iterator it(skins, ec), end; !ec && it != end; it.increment(ec))
    {
        const std::string name = utf8(it->path().filename());
        if (name.rfind(STAGING_PREFIX, 0) == 0) stale.push_back(it->path());
    }
    for (const fs::path& p : stale)
    {
        LL_INFOS("SkinImport") << "Removing a leftover import folder " << utf8(p) << LL_ENDL;
        slskin::removeTree(p);
    }
}

// static
void SLSkinImport::importZip(const std::string& zip_path, bool include_layout, done_cb_t done)
{
    Job job;
    job.kind = Job::ZIP;
    job.src = fsyspath(zip_path);
    job.source_label = zip_path;
    job.fallback_name = slskin::nameFromPath(zip_path);
    job.include_layout = include_layout;
    start(job, done);
}

// static
void SLSkinImport::importFolder(const std::string& dir, bool include_layout, done_cb_t done)
{
    const slskin::SourceGuess guess = slskin::classifySource(fsyspath(dir));
    if (guess.kind == slskin::SourceKind::FsSkinsDir || guess.kind == slskin::SourceKind::FsSkin ||
        guess.kind == slskin::SourceKind::FsTheme)
    {
        // A Firestorm skin: the user picks the theme in the Firestorm picker.
        LLSD key;
        key["dir"] = dir;
        key["include_layout"] = include_layout;
        LLFloaterReg::showInstance("skin_import_fs", key);
        return;
    }
    Job job;
    job.kind = Job::FOLDER;
    job.src = fsyspath(dir);
    job.source_label = dir;
    job.fallback_name = slskin::nameFromPath(utf8(guess.root.empty() ? job.src : guess.root));
    job.include_layout = include_layout;
    start(job, done);
}

// static
void SLSkinImport::importFirestorm(const std::string& skins_dir, const std::string& skin_folder, const std::string& skin_name,
                                   const std::string& theme_folder, const std::string& theme_name,
                                   const std::string& fallback_default, bool include_layout, done_cb_t done)
{
    Job job;
    job.kind = Job::FIRESTORM;
    job.skins_dir = fsyspath(skins_dir);
    job.skin_folder = skin_folder;
    job.theme_folder = theme_folder;
    job.fallback_default = fallback_default.empty() ? fs::path() : fs::path(fsyspath(fallback_default));
    job.source_viewer = "Firestorm";
    job.source_label = "Firestorm " + skin_name + " / " + theme_name + " (" + gDirUtilp->add(skins_dir, skin_folder) + ")";
    job.fallback_name = "FS " + skin_name + ((theme_name.empty() || theme_name == skin_name) ? std::string() : " - " + theme_name);
    job.include_layout = include_layout;
    start(job, done);
}

// static
std::vector<slskin::FsSkinsDir> SLSkinImport::findFirestormSkins()
{
    std::vector<fs::path> bases;
    auto env = [&bases](const char* name, const char* sub)
    {
        if (const auto v = LLStringUtil::getoptenv(name))
        {
            fs::path p = fsyspath(*v);
            if (sub) p /= sub;
            bases.push_back(p);
        }
    };
#if LL_WINDOWS
    env("ProgramW6432", nullptr);
    env("ProgramFiles", nullptr);
    env("ProgramFiles(x86)", nullptr);
    env("LOCALAPPDATA", "Programs");
    env("LOCALAPPDATA", nullptr);
#elif LL_DARWIN
    bases.push_back("/Applications");
    env("HOME", "Applications");
    env("HOME", "Downloads");
#else
    env("HOME", nullptr);
    env("HOME", "Applications");
    env("HOME", "apps");
    env("HOME", "Downloads");
    env("HOME", ".local/share");
    bases.push_back("/opt");
    bases.push_back("/usr/local");
#endif
    std::vector<slskin::FsSkinsDir> out;
    fs::path first_default;
    for (const fs::path& skins : slskin::findFsInstallSkinsDirs(bases))
    {
        slskin::FsSkinsDir d;
        d.dir = skins;
        d.label = "Firestorm (" + utf8(skins.parent_path()) + ")";
        d.skins = slskin::scanSkinsDir(skins);
        if (d.skins.empty()) continue;
        if (first_default.empty() && isDirectory(skins / "default")) first_default = skins / "default";
        out.push_back(d);
    }
    for (const slimport::SourceViewer& viewer : SLFloaterViewerImport::findInstalledViewers())
    {
        if (viewer.id != "firestorm") continue;
        slskin::FsSkinsDir d;
        d.dir = viewer.dir / "skins";
        if (!isDirectory(d.dir)) continue;
        d.user = true;
        d.label = viewer.label + " user skins (" + utf8(d.dir) + ")";
        d.skins = slskin::scanSkinsDir(d.dir);
        if (!d.skins.empty()) out.push_back(d);
    }
    LL_INFOS("SkinImport") << "Firestorm skin folders found: " << out.size() << LL_ENDL;
    return out;
}

// static
bool SLSkinImport::firestormSkinsIn(const std::string& dir, slskin::FsSkinsDir& out)
{
    const slskin::SourceGuess guess = slskin::classifySource(fsyspath(dir));
    if (guess.kind != slskin::SourceKind::FsSkinsDir && guess.kind != slskin::SourceKind::FsSkin &&
        guess.kind != slskin::SourceKind::FsTheme)
    {
        return false;
    }
    out = slskin::FsSkinsDir();
    out.dir = guess.skins_dir;
    out.label = "Picked folder (" + dir + ")";
    for (slskin::FsSkin skin : slskin::scanSkinsDir(guess.skins_dir))
    {
        if (guess.kind != slskin::SourceKind::FsSkinsDir && skin.folder != guess.skin_folder) continue;
        if (guess.kind == slskin::SourceKind::FsTheme)
        {
            std::vector<slskin::FsTheme> themes;
            for (const slskin::FsTheme& t : skin.themes)
            {
                if (t.folder == guess.theme_folder) themes.push_back(t);
            }
            skin.themes = themes;
        }
        if (!skin.themes.empty()) out.skins.push_back(skin);
    }
    return !out.skins.empty();
}
