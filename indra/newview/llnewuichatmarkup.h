// Native chat is another view over the classic URL registry, never an HTML parser.
#pragma once
#include "llurlregistry.h"
#include <string>
namespace slnewui
{
std::string chatMarkup(const std::string& text, const LLUrlLabelCallback& changed = &LLUrlRegistryNullCallback);
}
