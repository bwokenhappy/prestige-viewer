/**
 * @file lluictrlfactory.cpp
 * @brief Factory class for creating UI controls
 *
 * $LicenseInfo:firstyear=2003&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "linden_common.h"

#define LLUICTRLFACTORY_CPP
#include "lluictrlfactory.h"

#include "llxmlnode.h"

#include <fstream>
#include <list>
#include <set>
#include <unordered_map>
#include <boost/tokenizer.hpp>

// other library includes
#include "llcontrol.h"
#include "lldir.h"
#include "llfile.h"
#include "v4color.h"
#include "v3dmath.h"
#include "llquaternion.h"

// this library includes
#include "llpanel.h"

//-----------------------------------------------------------------------------

// UI Ctrl class for padding
class LLUICtrlLocate : public LLUICtrl
{
public:
    struct Params : public LLInitParam::Block<Params, LLUICtrl::Params>
    {
        Params()
        {
            name = "locate";
            tab_stop = false;
        }
    };

    LLUICtrlLocate(const Params& p) : LLUICtrl(p) {}
    virtual void draw() { }

};

static LLDefaultChildRegistry::Register<LLUICtrlLocate> r1("locate");

// Build time optimization, generate this once in .cpp file
template class LLUICtrlFactory* LLSingleton<class LLUICtrlFactory>::getInstance();

//-----------------------------------------------------------------------------
// LLUICtrlFactory()
//-----------------------------------------------------------------------------
LLUICtrlFactory::LLUICtrlFactory()
    : mDummyPanel(NULL) // instantiated when first needed
{
}

LLUICtrlFactory::~LLUICtrlFactory()
{
    // go ahead and leak mDummyPanel since this is static destructor time
    //delete mDummyPanel;
    //mDummyPanel = NULL;
}

void LLUICtrlFactory::loadWidgetTemplate(const std::string& widget_tag, LLInitParam::BaseBlock& block)
{
    std::string filename = gDirUtilp->add("widgets", widget_tag + ".xml");
    LLXMLNodePtr root_node;
    std::vector<std::string> search_paths =
        gDirUtilp->findSkinnedFilenames(LLDir::XUI, filename);

    if (search_paths.empty())
    {
        return;
    }

    // "en" version, the default-language version of the file.
    std::string base_filename = search_paths.front();
    if (!base_filename.empty())
    {
        LLUICtrlFactory::instance().pushFileName(base_filename);

        if (!LLXMLNode::getLayeredXMLNode(root_node, search_paths))
        {
            LL_WARNS() << "Couldn't parse widget from: " << base_filename << LL_ENDL;
            return;
        }
        LLXUIParser parser;
        parser.readXUI(root_node, block, base_filename);
        LLUICtrlFactory::instance().popFileName();
    }
}

//static
void LLUICtrlFactory::createChildren(LLView* viewp, LLXMLNodePtr node, const widget_registry_t& registry, LLXMLNodePtr output_node)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_UI;
    if (node.isNull()) return;

    for (LLXMLNodePtr child_node = node->getFirstChild(); child_node.notNull(); child_node = child_node->getNextSibling())
    {
        LLXMLNodePtr outputChild;
        if (output_node)
        {
            outputChild = output_node->createChild("", false);
        }

        if (!instance().createFromXML(child_node, viewp, LLStringUtil::null, registry, outputChild))
        {
            // child_node is not a valid child for the current parent
            std::string child_name = std::string(child_node->getName()->mString);
            if (LLDefaultChildRegistry::instance().getValue(child_name))
            {
                // This means that the registry assocaited with the parent widget does not have an entry
                // for the child widget
                // You might need to add something like:
                // static ParentWidgetRegistry::Register<ChildWidgetType> register("child_widget_name");
                LL_WARNS() << child_name << " is not a valid child of " << node->getName()->mString << LL_ENDL;
            }
            else
            {
                LL_WARNS() << "Could not create widget named " << child_node->getName()->mString << LL_ENDL;
            }
        }

        if (outputChild && !outputChild->mChildren && outputChild->mAttributes.empty() && outputChild->getValue().empty())
        {
            output_node->deleteChild(outputChild);
        }
    }

}

//-----------------------------------------------------------------------------
// getLayeredXMLNode()
//-----------------------------------------------------------------------------
namespace
{
    // Parsed + layered XUI trees keyed by (file, constraint). Entries are never handed out directly: callers
    // mutate the tree they get, so hits return an ordered deep copy. LRU-bounded; cleared on skin/language change.
    constexpr size_t MAX_XUI_CACHE_ENTRIES = 128;

    struct XUINodeCache
    {
        typedef std::pair<std::string, LLXMLNodePtr> entry_t;
        std::list<entry_t> lru;
        std::unordered_map<std::string, std::list<entry_t>::iterator> index;
        U32 generation = 0;
    };

    XUINodeCache& xuiNodeCache()
    {
        static XUINodeCache cache;
        return cache;
    }

    // #1599 runtime safety net: a layout file from a user skin (imported or hand edited) that does
    // not parse, has another root element, or drops a control our code looks up by name is
    // replaced by our own file for that window, and the swap is logged (tag "SkinImport").

    // Control names the code looks up (app_settings/xui_lookup_names.txt, one per line).
    const std::set<std::string>& lookedUpNames()
    {
        static std::set<std::string> names;
        static bool loaded = false;
        if (!loaded)
        {
            loaded = true;
            llifstream in(gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "xui_lookup_names.txt").c_str());
            std::string line;
            while (in.is_open() && std::getline(in, line))
            {
                LLStringUtil::trim(line);
                if (!line.empty() && line[0] != '#') names.insert(line);
            }
        }
        return names;
    }

    // The part after a user skin directory ("/xui/en/panel_x.xml"), or empty when `path` is not in one.
    std::string userSkinRemainder(const std::string& path)
    {
        for (const std::string* dir : { &gDirUtilp->getUserSkinDir(), &gDirUtilp->getUserDefaultSkinDir() })
        {
            if (!dir->empty() && path.size() > dir->size() && path.compare(0, dir->size(), *dir) == 0)
                return path.substr(dir->size());
        }
        return std::string();
    }

    // `paths` with each user-skin file replaced by the installed skin's or the default skin's
    // (dropped when we have none).
    std::vector<std::string> withoutUserSkin(const std::vector<std::string>& paths)
    {
        std::vector<std::string> out;
        for (const std::string& p : paths)
        {
            const std::string rest = userSkinRemainder(p);
            if (rest.empty())
            {
                out.push_back(p);
                continue;
            }
            for (const std::string* dir : { &gDirUtilp->getSkinDir(), &gDirUtilp->getDefaultSkinDir() })
            {
                if (gDirUtilp->fileExists(*dir + rest))
                {
                    out.push_back(*dir + rest);
                    break;
                }
            }
        }
        return out;
    }

    void collectNames(LLXMLNodePtr node, std::set<std::string>& names, S32 depth = 0)
    {
        if (node.isNull() || depth > 64) return;
        std::string name;
        if (node->getAttributeString("name", name) && !name.empty()) names.insert(name);
        for (LLXMLNodePtr child = node->getFirstChild(); child.notNull(); child = child->getNextSibling())
            collectNames(child, names, depth + 1);
    }

    // Empty when the user skin's base-language file (paths[0], already layered into `root`) can
    // stand in for ours; otherwise why not.
    std::string userSkinXuiProblem(const std::vector<std::string>& paths, LLXMLNodePtr root)
    {
        if (paths.empty() || userSkinRemainder(paths.front()).empty()) return std::string();
        const std::vector<std::string> ours_paths = withoutUserSkin({ paths.front() });
        if (ours_paths.empty()) return std::string();   // a file only the skin has: nothing to compare with
        LLXMLNodePtr ours;
        if (!LLXMLNode::parseFile(ours_paths.front(), ours, NULL) || ours.isNull()) return std::string();
        if (ours->getName() != root->getName())
            return std::string("its root element is <") + root->getName()->mString + ">, not <" + ours->getName()->mString + ">";
        std::string ours_name, theirs_name;
        ours->getAttributeString("name", ours_name);
        root->getAttributeString("name", theirs_name);
        if (!ours_name.empty() && ours_name != theirs_name) return "its root is named \"" + theirs_name + "\", not \"" + ours_name + "\"";
        std::set<std::string> ours_names, theirs_names;
        collectNames(ours, ours_names);
        collectNames(root, theirs_names);
        const std::set<std::string>& needed = lookedUpNames();
        for (const std::string& n : ours_names)
        {
            if (needed.count(n) && !theirs_names.count(n)) return "it has no control named \"" + n + "\"";
        }
        return std::string();
    }

    // LLXMLNode::getLayeredXMLNode with the fallback above.
    bool layeredWithUserSkinFallback(LLXMLNodePtr& root, const std::vector<std::string>& paths)
    {
        bool uses_user_skin = false;
        for (const std::string& p : paths) uses_user_skin = uses_user_skin || !userSkinRemainder(p).empty();
        const bool ok = LLXMLNode::getLayeredXMLNode(root, paths);
        if (!uses_user_skin) return ok;
        const std::string problem = ok ? userSkinXuiProblem(paths, root) : std::string("it does not parse");
        if (problem.empty()) return ok;
        const std::vector<std::string> ours = withoutUserSkin(paths);
        LL_WARNS("SkinImport") << "Skin layout file " << paths.front() << ": " << problem
                               << "; using the viewer's own layout instead" << LL_ENDL;
        if (ours.empty()) return false;
        return LLXMLNode::getLayeredXMLNode(root, ours);
    }
}

bool LLUICtrlFactory::getLayeredXMLNode(const std::string &xui_filename, LLXMLNodePtr& root,
                                        LLDir::ESkinConstraint constraint)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_UI;

    XUINodeCache& cache = xuiNodeCache();
    if (cache.generation != gDirUtilp->getSkinGeneration())
    {
        cache.lru.clear();
        cache.index.clear();
        cache.generation = gDirUtilp->getSkinGeneration();
    }

    std::string key = xui_filename;
    key += (constraint == LLDir::ALL_SKINS) ? '\1' : '\2';

    auto found = cache.index.find(key);
    if (found != cache.index.end())
    {
        cache.lru.splice(cache.lru.begin(), cache.lru, found->second);
        root = found->second->second->deepCopy();
        return true;
    }

    std::vector<std::string> paths =
        gDirUtilp->findSkinnedFilenames(LLDir::XUI, xui_filename, constraint);

    const bool cacheable = !paths.empty();
    if (paths.empty())
    {
        // sometimes whole path is passed in as filename
        paths.push_back(xui_filename);
    }

    if (!layeredWithUserSkinFallback(root, paths))
    {
        return false;
    }

    if (cacheable && root.notNull())
    {
        cache.lru.emplace_front(key, root->deepCopy());
        cache.index[key] = cache.lru.begin();
        if (cache.lru.size() > MAX_XUI_CACHE_ENTRIES)
        {
            cache.index.erase(cache.lru.back().first);
            cache.lru.pop_back();
        }
    }
    return true;
}


//-----------------------------------------------------------------------------
// saveToXML()
//-----------------------------------------------------------------------------
S32 LLUICtrlFactory::saveToXML(LLView* viewp, const std::string& filename)
{
    return 0;
}

//-----------------------------------------------------------------------------
//-----------------------------------------------------------------------------

LLView *LLUICtrlFactory::createFromXML(LLXMLNodePtr node, LLView* parent, const std::string& filename, const widget_registry_t& registry, LLXMLNodePtr output_node)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_UI;
    std::string ctrl_type = node->getName()->mString;
    LLStringUtil::toLower(ctrl_type);

    const LLWidgetCreatorFunc* funcp = registry.getValue(ctrl_type);
    if (funcp == NULL)
    {
        return NULL;
    }

    if (parent == NULL)
    {
        if (mDummyPanel == NULL)
        {
            LLPanel::Params p;
            mDummyPanel = create<LLPanel>(p);
        }
        parent = mDummyPanel;
    }
    LLView *view = (*funcp)(node, parent, output_node);

    return view;
}

std::string LLUICtrlFactory::getCurFileName()
{
    return mFileNames.empty() ? "" : mFileNames.back();
}


void LLUICtrlFactory::pushFileName(const std::string& name)
{
    // Here we seem to be looking for the default language file ("en") rather
    // than the localized one, if any.
    // memoized: the lookup stats several skin dirs and this runs for every panel reference built
    constexpr size_t MAX_BASE_NAME_ENTRIES = 1024;
    static std::unordered_map<std::string, std::string> base_names;
    static U32 base_names_generation = 0;
    if (base_names_generation != gDirUtilp->getSkinGeneration() || base_names.size() >= MAX_BASE_NAME_ENTRIES)
    {
        base_names.clear();
        base_names_generation = gDirUtilp->getSkinGeneration();
    }
    auto it = base_names.find(name);
    if (it == base_names.end())
    {
        it = base_names.emplace(name, gDirUtilp->findSkinnedFilenameBaseLang(LLDir::XUI, name)).first;
    }
    mFileNames.push_back(it->second);
}

void LLUICtrlFactory::popFileName()
{
    mFileNames.pop_back();
}

//static
void LLUICtrlFactory::setCtrlParent(LLView* view, LLView* parent, S32 tab_group)
{
    if (tab_group == S32_MAX) tab_group = parent->getLastTabGroup();
    parent->addChild(view, tab_group);
}

//static
void LLUICtrlFactory::copyName(LLXMLNodePtr src, LLXMLNodePtr dest)
{
    dest->setName(src->getName()->mString);
}

template<typename T>
const LLInitParam::BaseBlock& get_empty_param_block()
{
    static typename T::Params params;
    return params;
}

// adds a widget and its param block to various registries
//static
void LLUICtrlFactory::registerWidget(const std::type_info* widget_type, const std::type_info* param_block_type, const std::string& name)
{
    // associate parameter block type with template .xml file
    std::string* existing_name = LLWidgetNameRegistry::instance().getValue(param_block_type);
    if (existing_name != NULL)
    {
        if(*existing_name != name)
        {
            std::cerr << "Duplicate entry for T::Params, try creating empty param block in derived classes that inherit T::Params" << std::endl;
            // forcing crash here
            char* foo = 0;
            *foo = 1;
        }
        else
        {
            // widget already registered this name
            return;
        }
    }

    LLWidgetNameRegistry::instance().defaultRegistrar().add(param_block_type, name);
    //FIXME: comment this in when working on schema generation
    //LLWidgetTypeRegistry::instance().defaultRegistrar().add(tag, widget_type);
    //LLDefaultParamBlockRegistry::instance().defaultRegistrar().add(widget_type, &get_empty_param_block<T>);
}


