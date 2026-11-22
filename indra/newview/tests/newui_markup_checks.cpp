// Actual LLUrlRegistry + native view adapter. Public synthetic text only, no viewer or network idle.
#include "linden_common.h"
#include "llnewuichatmarkup.h"
#include "llnewuikitstyle.h"
#include "llfontgl.h"
#include <cstdlib>
#include <iostream>
// The low-level URL library delegates anonymisation to the viewer; no avatar URL
// or live identity is used in this fixture. Keep that external policy seam inert.
class LLAvatarName;
const std::string& rlvGetAnonym(const LLAvatarName&)
{
    static const std::string value="Anonymous Resident";
    return value;
}
static void require(bool ok,const char* message) { if(!ok) { std::cerr << "FAIL: " << message << '\n'; std::exit(1); } }
int main()
{
    // LLStyle constructs a default font handle even for text-only URL matching.
    // Create the registry without GL textures; this test never draws through LLFontGL.
    LLFontGL::initClass(96.f,1.f,1.f,".",false);
    const auto plain=slnewui::chatMarkup("<nolink>https://example.invalid/plain</nolink>");
    require(plain=="https://example.invalid/plain","classic nolink label, no link or raw tags");
    const auto mixed=slnewui::chatMarkup("<nolink>https://example.invalid/plain</nolink> https://example.invalid/link");
    require(mixed.find("chat-url=\"https://example.invalid/link\"")!=std::string::npos,"classic HTTP link metadata");
    require(mixed.find("chat-url=\"https://example.invalid/plain\"")==std::string::npos,"nolink does not become clickable");
    const auto hostile=slnewui::chatMarkup("<script>bad</script> {{secret}} & \"text\"");
    require(hostile.find("<script>")==std::string::npos && hostile.find("{{")==std::string::npos,"no markup or model-expression injection");
    require(hostile.find("&#123;")!=std::string::npos,"escaped binding delimiters");
    const auto slurl=slnewui::chatMarkup("secondlife://Example/128/128/25");
    require(slurl.find("chat-url=")!=std::string::npos && slurl.find("Example")!=std::string::npos,"classic SLURL label/target");
    const auto bracket=slnewui::chatMarkup("[https://example.invalid/path Example label]");
    require(bracket.find("chat-url=")!=std::string::npos && bracket.find("Example label")!=std::string::npos,"classic bracket URL label");
    std::cout << "PASS: real URL registry nolink/HTTP/SLURL/bracket labels; safe RML and binding escaping\n";
}
