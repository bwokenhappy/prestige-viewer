#include "linden_common.h"
#include "llnewuichatmarkup.h"
#include "llnewuikitstyle.h"

std::string slnewui::chatMarkup(const std::string& text, const LLUrlLabelCallback& changed)
{
    const size_t scan = LLUrlRegistry::getUrlScanLength(text);
    std::string rest = text.substr(0, scan), result;
    LLUrlSearchState state;
    LLUrlMatch match;
    unsigned count = 0;
    while (count++ < 100 && LLUrlRegistry::instance().findUrl(rest, match, changed, false, false, &state))
    {
        const size_t first = match.getStart(), end = size_t(match.getEnd()) + 1;
        if (first > rest.size() || end <= first || end > rest.size()) break;
        result += LLNewUIKit::escapeText(rest.substr(0, first));
        const auto style = match.getStyle();
        const std::string label = LLNewUIKit::escapeText(match.getLabel());
        if (style.is_link.isProvided() && !style.is_link()) result += label;
        else result += "<span class=\"chatlink\" chat-url=\"" + LLNewUIKit::escapeText(match.getUrl()) + "\">" + label + "</span>";
        rest.erase(0, end); state.advance(end);
    }
    return result + LLNewUIKit::escapeText(rest) + LLNewUIKit::escapeText(text.substr(scan));
}
