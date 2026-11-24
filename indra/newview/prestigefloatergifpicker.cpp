/**
 * @file prestigefloatergifpicker.cpp
 * @brief Prestige GIF picker (GIPHY / KLIPY) for nearby chat and IMs.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Prestige Viewer Source Code
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "prestigefloatergifpicker.h"

#include "llapp.h"
#include "llcallbacklist.h"
#include "llcombobox.h"
#include "llcommandhandler.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "lldir.h"
#include "llfile.h"
#include "llfloaterimsessiontab.h"
#include "llfloaterreg.h"
#include "llhttpconstants.h"
#include "llmediactrl.h"
#include "llsearcheditor.h"
#include "lltextbox.h"
#include "lluri.h"
#include "llviewercontrol.h"

#include <algorithm>
#include <initializer_list>

namespace
{
    const char* GIPHY_KEY_SETTING    = "PrestigeGiphyApiKey";
    const char* KLIPY_KEY_SETTING    = "PrestigeKlipyApiKey";
    const char* PROVIDER_SETTING     = "PrestigeGifProvider";
    const char* PAGE_FILE            = "prestige_gif_picker.html";

    constexpr S32 RESULT_COUNT       = 24;
    constexpr F32 RESULTS_FRESH_SECS = 300.f;    // reuse shown results this long
    constexpr size_t MAX_URL_LENGTH  = 2048;

    std::string get_key(PrestigeFloaterGifPicker::EProvider provider)
    {
        std::string key = gSavedSettings.getString(provider == PrestigeFloaterGifPicker::PROVIDER_KLIPY
                                                   ? KLIPY_KEY_SETTING : GIPHY_KEY_SETTING);
        LLStringUtil::trim(key);
        return key;
    }

    // KLIPY wants an id for "the user in your system". A random one per session: it
    // ties nothing to the avatar or the computer.
    const std::string& klipy_customer_id()
    {
        static const std::string id = LLUUID::generateNewID().asString();
        return id;
    }

    // Only plain https links make it onto the page or into chat.
    bool is_safe_url(const std::string& url)
    {
        if (url.size() > MAX_URL_LENGTH || url.compare(0, 8, "https://") != 0 || url.size() <= 8)
        {
            return false;
        }
        for (char c : url)
        {
            const unsigned char uc = (unsigned char)c;
            if (uc <= 0x20 || uc >= 0x7f || c == '"' || c == '\'' || c == '<' || c == '>' || c == '\\' || c == '`')
            {
                return false;
            }
        }
        return true;
    }

    std::string first_safe_url(std::initializer_list<const LLSD*> candidates)
    {
        for (const LLSD* candidate : candidates)
        {
            if (candidate && candidate->isString())
            {
                const std::string url = candidate->asString();
                if (is_safe_url(url))
                {
                    return url;
                }
            }
        }
        return std::string();
    }

    std::string html_escape(const std::string& text)
    {
        std::string out;
        out.reserve(text.size());
        for (char c : text)
        {
            switch (c)
            {
            case '&':  out += "&amp;";  break;
            case '<':  out += "&lt;";   break;
            case '>':  out += "&gt;";   break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#39;";  break;
            default:
                if ((unsigned char)c >= 0x20 || c == '\t')
                {
                    out += c;
                }
                break;
            }
        }
        return out;
    }

    // GIPHY: { "data": [ { "title", "images": { "fixed_height_small": { "url" }, "downsized": { "url" },
    // "original": { "url" } } } ], "meta": { ... } }
    void parse_giphy(const LLSD& reply, std::vector<PrestigeFloaterGifPicker::Result>& out)
    {
        const LLSD& data = reply["data"];
        if (!data.isArray())
        {
            return;
        }
        for (LLSD::array_const_iterator it = data.beginArray(); it != data.endArray(); ++it)
        {
            const LLSD& images = (*it)["images"];
            PrestigeFloaterGifPicker::Result result;
            result.mThumbURL = first_safe_url({ &images["fixed_height_small"]["url"],
                                                &images["fixed_height"]["url"],
                                                &images["downsized"]["url"] });
            result.mGifURL = first_safe_url({ &images["downsized"]["url"], &images["original"]["url"] });
            // The link works without GIPHY's tracking parameters, and reads better in chat
            const size_t query = result.mGifURL.find('?');
            if (query != std::string::npos)
            {
                result.mGifURL.erase(query);
            }
            result.mTitle = (*it)["title"].asString();
            if (!result.mThumbURL.empty() && !result.mGifURL.empty())
            {
                out.push_back(result);
            }
        }
    }

    // KLIPY: { "result": true, "data": { "data": [ { "title", "slug", "type",
    // "file": { "hd" | "md" | "sm" | "xs": { "gif" | "webp" | ...: { "url", "width", "height" } } } } ],
    // "current_page", "per_page", "has_next" } }
    void parse_klipy(const LLSD& reply, std::vector<PrestigeFloaterGifPicker::Result>& out)
    {
        const LLSD& data = reply["data"]["data"];
        if (!data.isArray())
        {
            return;
        }
        for (LLSD::array_const_iterator it = data.beginArray(); it != data.endArray(); ++it)
        {
            if ((*it)["type"].asString() == "ad")
            {
                continue;
            }
            const LLSD& file = (*it)["file"];
            PrestigeFloaterGifPicker::Result result;
            result.mThumbURL = first_safe_url({ &file["sm"]["webp"]["url"], &file["sm"]["gif"]["url"],
                                                &file["xs"]["gif"]["url"], &file["md"]["gif"]["url"] });
            result.mGifURL = first_safe_url({ &file["md"]["gif"]["url"], &file["hd"]["gif"]["url"],
                                              &file["sm"]["gif"]["url"] });
            result.mTitle = (*it)["title"].asString();
            if (!result.mThumbURL.empty() && !result.mGifURL.empty())
            {
                out.push_back(result);
            }
        }
    }

    // A file:// URL for a local path (forward slashes, everything but plain characters escaped).
    std::string file_url_for_path(std::string path)
    {
        std::replace(path.begin(), path.end(), '\\', '/');
        static const char hex[] = "0123456789ABCDEF";
        std::string url = "file://";
        if (path.empty() || path[0] != '/')
        {
            url += '/';
        }
        for (char c : path)
        {
            const unsigned char uc = (unsigned char)c;
            const bool plain = (uc >= 'A' && uc <= 'Z') || (uc >= 'a' && uc <= 'z') || (uc >= '0' && uc <= '9')
                               || c == '-' || c == '_' || c == '.' || c == '~' || c == '/' || c == ':';
            if (plain)
            {
                url += c;
            }
            else
            {
                url += '%';
                url += hex[uc >> 4];
                url += hex[uc & 0x0f];
            }
        }
        return url;
    }
}

// secondlife:///app/prestigegif/pick/<page>/<index>: a thumbnail on the picker's page.
// Only the picker's own (trusted) browser may use it.
class PrestigeGifPickHandler : public LLCommandHandler
{
public:
    PrestigeGifPickHandler() : LLCommandHandler("prestigegif", UNTRUSTED_BLOCK) {}

    bool handle(const LLSD& params, const LLSD& /*query_map*/, const std::string& /*grid*/, LLMediaCtrl* /*web*/) override
    {
        if (params.size() < 3 || params[0].asString() != "pick")
        {
            return false;
        }
        PrestigeFloaterGifPicker* picker = LLFloaterReg::findTypedInstance<PrestigeFloaterGifPicker>("prestige_gif_picker");
        if (!picker || !picker->getVisible())
        {
            return true;
        }
        const S32 page = params[1].asInteger();
        const S32 index = params[2].asInteger();
        if (page >= 0 && index >= 0)
        {
            picker->pick((U32)page, (U32)index);
        }
        return true;
    }
};
PrestigeGifPickHandler gPrestigeGifPickHandler;

PrestigeFloaterGifPicker::PrestigeFloaterGifPicker(const LLSD& key)
:   LLFloater(key)
{
}

PrestigeFloaterGifPicker::~PrestigeFloaterGifPicker() = default;

bool PrestigeFloaterGifPicker::postBuild()
{
    mSearchEditor = getChild<LLSearchEditor>("gif_search");
    mSearchEditor->setCommitOnFocusLost(false);
    mSearchEditor->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSearchCommit(); });

    mProviderCombo = getChild<LLComboBox>("gif_provider");
    mProviderCombo->setCommitCallback([this](LLUICtrl*, const LLSD&) { onProviderCommit(); });

    mBrowser = getChild<LLMediaCtrl>("gif_results");
    mStatusText = getChild<LLTextBox>("gif_status");

    return true;
}

void PrestigeFloaterGifPicker::onOpen(const LLSD& key)
{
    mSessionID = key.asUUID();
    updateProviderControls();
    if (!hasAnyKey())
    {
        mResults.clear();
        mHaveResults = false;
        showPage(getString("no_key"));
        setStatus(LLStringUtil::null);
        return;
    }
    runSearch(false);
    mSearchEditor->setFocus(true);
}

void PrestigeFloaterGifPicker::onClose(bool app_quitting)
{
    if (app_quitting)
    {
        return;
    }
    // Free the browser while the picker is closed; the page is rebuilt on open. Not right
    // now: a pick closes the picker from inside the browser's own click event.
    LLHandle<LLFloater> handle = getHandle();
    doOnIdleOneTime([handle]()
    {
        PrestigeFloaterGifPicker* self = dynamic_cast<PrestigeFloaterGifPicker*>(handle.get());
        if (self && !self->getVisible() && self->mBrowser)
        {
            self->mBrowser->unloadMediaSource();
        }
    });
}

// static
bool PrestigeFloaterGifPicker::hasKey(EProvider provider)
{
    return !get_key(provider).empty();
}

// static
bool PrestigeFloaterGifPicker::hasAnyKey()
{
    return hasKey(PROVIDER_GIPHY) || hasKey(PROVIDER_KLIPY);
}

// static
void PrestigeFloaterGifPicker::toggleFor(const LLUUID& session_id)
{
    PrestigeFloaterGifPicker* picker = LLFloaterReg::findTypedInstance<PrestigeFloaterGifPicker>("prestige_gif_picker");
    if (picker && picker->getVisible() && picker->mSessionID == session_id)
    {
        picker->closeFloater();
        return;
    }
    LLFloaterReg::showInstance("prestige_gif_picker", LLSD(session_id), true);
}

PrestigeFloaterGifPicker::EProvider PrestigeFloaterGifPicker::currentProvider() const
{
    const bool giphy = hasKey(PROVIDER_GIPHY);
    const bool klipy = hasKey(PROVIDER_KLIPY);
    if (giphy && klipy)
    {
        return gSavedSettings.getString(PROVIDER_SETTING) == "klipy" ? PROVIDER_KLIPY : PROVIDER_GIPHY;
    }
    return klipy ? PROVIDER_KLIPY : PROVIDER_GIPHY;
}

std::string PrestigeFloaterGifPicker::providerName(EProvider provider) const
{
    return provider == PROVIDER_KLIPY ? std::string("KLIPY") : std::string("GIPHY");
}

void PrestigeFloaterGifPicker::updateProviderControls()
{
    // The switch only shows when both services have a key; the search box then
    // leaves room for it, otherwise it takes the whole row.
    const bool both = hasKey(PROVIDER_GIPHY) && hasKey(PROVIDER_KLIPY);
    const EProvider provider = currentProvider();
    mProviderCombo->setVisible(both);
    mProviderCombo->setValue(LLSD(provider == PROVIDER_KLIPY ? "klipy" : "giphy"));

    LLRect search_rect = mSearchEditor->getRect();
    const LLRect& combo_rect = mProviderCombo->getRect();
    search_rect.mRight = both ? combo_rect.mLeft - 4 : combo_rect.mRight;
    mSearchEditor->setShape(search_rect);

    LLStringUtil::format_map_t args;
    args["[SERVICE]"] = providerName(provider);
    mSearchEditor->setLabel(getString("search_label", args));
}

void PrestigeFloaterGifPicker::onSearchCommit()
{
    runSearch(false);
}

void PrestigeFloaterGifPicker::onProviderCommit()
{
    const std::string value = mProviderCombo->getValue().asString();
    gSavedSettings.setString(PROVIDER_SETTING, value == "klipy" ? "klipy" : "giphy");
    updateProviderControls();
    runSearch(false);
}

void PrestigeFloaterGifPicker::runSearch(bool force)
{
    const EProvider provider = currentProvider();
    const std::string key = get_key(provider);
    if (key.empty())
    {
        showPage(getString("no_key"));
        return;
    }

    std::string query = mSearchEditor->getText();
    LLStringUtil::trim(query);

    if (!force && mHaveResults && provider == mShownProvider && query == mShownQuery
        && mShownAge.getElapsedTimeF32() < RESULTS_FRESH_SECS)
    {
        if (!mBusy)
        {
            showPage(mResults.empty() ? getString("no_results") : LLStringUtil::null);
            LLStringUtil::format_map_t args;
            args["[SERVICE]"] = providerName(provider);
            setStatus(getString("powered_by", args));
        }
        return;
    }

    // The key goes in the URL (GIPHY: query string, KLIPY: path); keep it out of the log
    const std::string escaped_key = LLURI::escape(key);
    LLCoreHttpUtil::addLogRedaction(key);
    LLCoreHttpUtil::addLogRedaction(escaped_key);

    std::string url;
    const std::string count = llformat("%d", RESULT_COUNT);
    if (provider == PROVIDER_GIPHY)
    {
        url = query.empty()
            ? "https://api.giphy.com/v1/gifs/trending?api_key=" + escaped_key
            : "https://api.giphy.com/v1/gifs/search?api_key=" + escaped_key + "&q=" + LLURI::escape(query);
        url += "&limit=" + count + "&rating=pg-13";
    }
    else
    {
        url = "https://api.klipy.com/api/v1/" + escaped_key
            + (query.empty() ? std::string("/gifs/trending?") : "/gifs/search?q=" + LLURI::escape(query) + "&");
        url += "per_page=" + count + "&page=1&rating=pg-13&customer_id=" + LLURI::escape(klipy_customer_id());
    }

    const U32 serial = ++mRequestSerial;
    mBusy = true;
    LLStringUtil::format_map_t args;
    args["[SERVICE]"] = providerName(provider);
    setStatus(getString(query.empty() ? "loading_trending" : "searching", args));

    LLHandle<LLFloater> handle = getHandle();
    LLCoros::instance().launch("PrestigeFloaterGifPicker::fetchCoro",
        [handle, serial, provider, query, url]() { fetchCoro(handle, serial, provider, query, url); });
}

// static
void PrestigeFloaterGifPicker::fetchCoro(LLHandle<LLFloater> handle, U32 serial, EProvider provider,
                                         std::string query, std::string url)
{
    LLCore::HttpRequest::policy_t policy(LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter(
        new LLCoreHttpUtil::HttpCoroutineAdapter("PrestigeGifPicker", policy));
    LLCore::HttpRequest::ptr_t request(new LLCore::HttpRequest);
    LLCore::HttpOptions::ptr_t options(new LLCore::HttpOptions);
    LLCore::HttpHeaders::ptr_t headers(new LLCore::HttpHeaders);

    options->setRetries(0);
    options->setTimeout(15);
    options->setTransferTimeout(15);
    options->setFollowRedirects(true);
    headers->append(HTTP_OUT_HEADER_ACCEPT, HTTP_CONTENT_JSON);

    LLSD reply = adapter->getJsonAndSuspend(request, url, options, headers);
    if (LLApp::isExiting())
    {
        return;
    }

    PrestigeFloaterGifPicker* self = dynamic_cast<PrestigeFloaterGifPicker*>(handle.get());
    if (!self || serial != self->mRequestSerial)
    {
        return;   // closed, or a newer search replaced this one
    }

    const LLSD& http_results = reply[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
    const LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(http_results);

    std::vector<Result> results;
    std::string error;
    LLStringUtil::format_map_t args;
    args["[SERVICE]"] = self->providerName(provider);
    if (!status)
    {
        const S32 code = status.isHttpStatus() ? (S32)status.getType() : 0;
        if (code == 401 || code == 403)
        {
            error = self->getString("error_key", args);
        }
        else if (code == 429)
        {
            error = self->getString("error_limit", args);
        }
        else
        {
            error = self->getString("error_other", args);
        }
        // The status only; never the URL (it holds the key)
        LL_INFOS("GifPicker") << self->providerName(provider) << " request failed: " << status.toTerseString() << LL_ENDL;
    }
    else if (provider == PROVIDER_GIPHY)
    {
        parse_giphy(reply, results);
    }
    else
    {
        parse_klipy(reply, results);
    }

    self->onResults(serial, provider, query, results, error);
}

void PrestigeFloaterGifPicker::onResults(U32 serial, EProvider provider, const std::string& query,
                                         const std::vector<Result>& results, const std::string& error)
{
    if (serial != mRequestSerial)
    {
        return;
    }
    mBusy = false;

    LLStringUtil::format_map_t args;
    args["[SERVICE]"] = providerName(provider);
    if (!error.empty())
    {
        setStatus(error);
        if (!mHaveResults)
        {
            showPage(error);
        }
        return;   // otherwise keep what is shown
    }

    mResults = results;
    mHaveResults = true;
    mShownProvider = provider;
    mShownQuery = query;
    mShownAge.reset();

    showPage(mResults.empty() ? getString("no_results") : LLStringUtil::null);
    setStatus(getString("powered_by", args));
}

void PrestigeFloaterGifPicker::showPage(const std::string& message)
{
    if (!mBrowser || !getVisible())
    {
        return;   // closed: a late reply must not start the browser; onOpen shows the page
    }
    const U32 page = ++mPageSerial;   // clicks on an older page are ignored

    std::string html =
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; img-src https:; style-src 'unsafe-inline'\">"
        "<style>"
        "html,body{margin:0;padding:0;background:#1F2023;color:#A8A9AD;font:13px sans-serif;}"
        ".grid{display:flex;flex-wrap:wrap;gap:4px;padding:4px;}"
        ".grid a{flex:1 1 auto;height:100px;min-width:60px;max-width:100%;display:block;"
        "background:#17181B;border-radius:4px;overflow:hidden;outline:none;}"
        ".grid a:hover,.grid a:focus{box-shadow:inset 0 0 0 2px #F28C28;}"
        ".grid img{height:100px;width:100%;object-fit:cover;display:block;}"
        ".msg{padding:24px 16px;text-align:center;}"
        "</style></head><body>";

    if (!message.empty())
    {
        html += "<div class=\"msg\">" + html_escape(message) + "</div>";
    }
    else
    {
        html += "<div class=\"grid\">";
        for (size_t i = 0; i < mResults.size(); ++i)
        {
            const Result& result = mResults[i];
            const std::string title = html_escape(result.mTitle);
            html += llformat("<a href=\"secondlife:///app/prestigegif/pick/%u/%u\" title=\"", page, (U32)i);
            html += title + "\"><img src=\"" + html_escape(result.mThumbURL) + "\" alt=\"" + title + "\"></a>";
        }
        html += "</div>";
    }
    html += "</body></html>";

    const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_CACHE, PAGE_FILE);
    {
        llofstream out(path.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
        if (!out.is_open())
        {
            LL_WARNS("GifPicker") << "Could not write the picker page to the cache folder" << LL_ENDL;
            return;
        }
        out << html;
    }
    // A new query each time, so the browser loads the new page instead of its cached copy
    mBrowser->navigateTo(file_url_for_path(path) + llformat("?p=%u", page), HTTP_CONTENT_TEXT_HTML);
}

void PrestigeFloaterGifPicker::setStatus(const std::string& text)
{
    if (mStatusText)
    {
        mStatusText->setText(text);
    }
}

void PrestigeFloaterGifPicker::pick(U32 page, U32 index)
{
    if (page != mPageSerial || index >= mResults.size())
    {
        return;
    }
    const std::string url = mResults[index].mGifURL;

    LLFloaterIMSessionTab* conversation = LLFloaterIMSessionTab::findConversation(mSessionID);
    if (!conversation)
    {
        setStatus(getString("error_conversation"));
        return;
    }
    conversation->insertIntoInput(url);
    closeFloater();
}
