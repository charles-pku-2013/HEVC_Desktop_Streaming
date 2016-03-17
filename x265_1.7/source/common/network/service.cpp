#include "service.hpp"

// inline //!! cannot be inline
ClientInfo::~ClientInfo()
{
    for( auto &v : services )
        (v.second)->terminate();
}



