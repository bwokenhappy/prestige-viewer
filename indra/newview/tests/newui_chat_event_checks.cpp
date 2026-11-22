// CPU only: the actual Nearby model publishes before notifying, without a poll.
#include "llnearbychatlog.h"
// Compile the actual model, omitting only the unrelated viewer-wide PCH.
#define LL_LLVIEWERPRECOMPILEDHEADERS_H
#include "../llnearbychatlog.cpp"
#include <boost/signals2/connection.hpp>
#include <cstdlib>
#include <iostream>

void require(bool ok,const char* message)
{
    if (!ok) { std::cerr<<"FAIL: "<<message<<'\n'; std::exit(1); }
}
int main()
{
    auto& log=LLNearbyChatLog::instance();
    unsigned events=0;
    const auto initial=log.revision();
    bool inAdd=false;
    std::string expected;
    boost::signals2::scoped_connection connection=log.addChangedCallback([&]
    {
        require(inAdd,"notification is synchronous with add, not a timer");
        ++events;
        require(log.revision()==initial+events,"revision published before event");
        require(log.at(log.size()-1).text==expected,"line published before event");
    });
    for (unsigned i=0;i<450;++i)
    {
        expected="Synthetic message "+std::to_string(i);
        inAdd=true; log.add("Synthetic sender",LLUUID::null,expected,"12:34"); inAdd=false;
        require(events==i+1,"one notification per line, including ring wrap");
    }
    require(log.size()==log.kCapacity,"model remains bounded");
    connection.disconnect();
    log.add("Synthetic sender",LLUUID::null,"Synthetic message","12:34");
    require(events==450,"destroyed view no longer receives messages");
    std::cout<<"PASS: actual Nearby publication/event order, immediate delivery, 450-message wrap, disconnected view\n";
}
