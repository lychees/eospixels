#include <eosiolib/currency.hpp>
#include <eosiolib/eosio.hpp>
#include <eosiolib/singleton.hpp>
#include <vector>

#include "kyubey.hpp"

class community : public kyubey {
 public:
  community(account_name self)
      : kyubey(self){

      }

    // the first argument of multi_index must be the name of the table
    //typedef multi_index<N(vote), vote> vote;

    struct toplist {
        std::vector<account_name> name; 
    };
    //typedef singleton<N(candidate), candidate> candidates;

    //@abi table candidate i64
    struct candidate {
        uint64_t in;
        uint64_t out;
    };
    typedef eosio::singleton<N(candidate), candidate> candidates;

    //@abi table voter i64
    struct vote {
        account_name candidate;
        uint64_t count;
        uint64_t primary_key() const { return candidate; }
    };
    typedef eosio::multi_index<N(vote), vote> vote_index;


    void vote(account_name from, account_name to, uint64_t count) {
        vote_index votes(_self, from); 
        auto itr = votes.find(to);  

        if (itr == votes.end()) {
            votes.emplace(_self, [&](auto& v) {    
                v.candidate = to;
                v.count = count;
            });            
        } else {
            votes.modify(itr, 0, [&](auto &v) {
                v.count += count;
            });
        }
    }
    /*
    void unstack() {
    };  */
};
