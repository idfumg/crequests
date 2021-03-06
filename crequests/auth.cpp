#include "auth.h"
#include <ostream>

namespace crequests {


    auth_t auth_t::from_string(const string_t& str) {
        const auto ind = str.find(":");
        if (ind != string_t::npos)
            return
                auth_t {
                    login_t{str.substr(0, ind)},
                    password_t{str.substr(ind + 1)}
                };
        
        throw std::runtime_error("unexpected auth_t str: " + str);
    }
    
    string_t auth_t::to_string() const {
        return this->first.value() + ":" + this->second.value();
    }

    std::ostream& operator<<(std::ostream& out, const auth_t& auth) {
        out << "auth_t(" << auth.to_string() << ")";
        return out;
    }

    auth_t operator "" _auth(const char* val, size_t) {
        return auth_t::from_string(val);
    }


} /* namespace crequests */
