#include "EOSPixels.hpp"

#include <cmath>
#include <eosiolib/action.hpp>
#include <eosiolib/asset.hpp>

#include "memo.hpp"
#include "types.hpp"

#define EOS_SYMBOL S(4, EOS)
#define PXL_SYMBOL S(4, PXL)

using namespace eosio;
using namespace std;

template <uint64_t A, typename B, typename... C>
void clear_table(multi_index<A, B, C...> *table, uint16_t limit) {
  auto it = table->begin();
  uint16_t count = 0;
  while (it != table->end() && count < limit) {
    it = table->erase(it);
    count++;
  }
}

// template <uint64_t A, typename B, typename... C>
// void clear_table_r(multi_index<A, B, C...> *table, uint16_t limit) {
//   auto it = table->end();
//   uint16_t count = 0;
//   while (it != table->begin() && count < limit) {
//     it--;
//     it = table->erase(it);
//     count++;
//   }
// }

void eospixels::clearpixels(uint16_t count, uint16_t nonce) {
  require_auth(TEAM_ACCOUNT);

  auto itr = canvases.begin();
  eosio_assert(itr != canvases.end(), "no canvas exists");

  pixel_store pixels(_self, itr->id);
  clear_table(&pixels, count);
}

// void eospixels::clearpixelsr(uint16_t count, uint16_t nonce) {
//   require_auth(TEAM_ACCOUNT);

//   auto itr = canvases.begin();
//   eosio_assert(itr != canvases.end(), "no canvas exists");

//   pixel_store pixels(_self, itr->id);
//   clear_table_r(&pixels, count);
// }

void eospixels::clearaccts(uint16_t count, uint16_t nonce) {
  require_auth(TEAM_ACCOUNT);

  clear_table(&accounts, count);
}

void eospixels::clearcanvs(uint16_t count, uint16_t nonce) {
  require_auth(TEAM_ACCOUNT);

  clear_table(&canvases, count);
}

void eospixels::resetquota() {
  require_auth(TEAM_ACCOUNT);

  auto guardItr = guards.begin();
  if (guardItr == guards.end()) {
    guards.emplace(_self, [&](guard &grd) {
      grd.id = 0;
      grd.quota = WITHDRAW_QUOTA;
    });
  } else {
    guards.modify(guardItr, 0, [&](guard &grd) { grd.quota = WITHDRAW_QUOTA; });
  }
}

// FIXME change allPixels to a reference?
void eospixels::drawPixel(pixel_store &allPixels,
                          const st_pixelOrder &pixelOrder,
                          st_transferContext &ctx) {
  auto loc = pixelOrder.location();

  auto pixelRowItr = allPixels.find(loc.row);

  // TODO extract this into its own method
  // Emplace & initialize empty row if it doesn't already exist
  bool hasRow = pixelRowItr != allPixels.end();
  if (!hasRow) {
    pixelRowItr = allPixels.emplace(_self, [&](pixel_row &pixelRow) {
      pixelRow.row = loc.row;
      pixelRow.initialize_empty_pixels();
    });
  }

  auto pixels = pixelRowItr->pixels;
  auto pixel = pixels[loc.col];

  auto result = ctx.purchase(pixel, pixelOrder);
  if (result.isSkipped) {
    return;
  }

  allPixels.modify(pixelRowItr, 0, [&](pixel_row &pixelRow) {
    pixelRow.pixels[loc.col] = {pixelOrder.color, pixel.nextPriceCounter(),
                                ctx.purchaser};
  });

  if (!result.isFirstBuyer) {
    deposit(pixel.owner, result.ownerEarningScaled);
  }
}

bool eospixels::isValidReferrer(account_name name) {
  auto it = accounts.find(name);

  if (it == accounts.end()) {
    return false;
  }

  // referrer must have painted at least one pixel
  return it->pixelsDrawn > 0;
}


const double ANTI_BUY_FEE_RATIO = 0.90; // 90%
const double BUY_START = 1538395200 - 24*60*60; // 09/30/2018 @ 12:00pm (UTC)
const double BUY_END =   1538395200 + 24*60*60; // 10/02/2018 @ 12:00pm (UTC)

void eospixels::charge_buy_fee(asset& quantity) {
    const double NOW = now();
    double fee_ratio = 0;
    if (NOW >= BUY_END){
    } else if (NOW <= BUY_START) {
        fee_ratio += ANTI_BUY_FEE_RATIO;
    } else {
        fee_ratio = ANTI_BUY_FEE_RATIO * (BUY_END-NOW) / (BUY_END-BUY_START);
    }
    auto fee = asset(quantity.amount * fee_ratio, quantity.symbol);

    if (fee.amount > 0) {
      action(permission_level{_self, N(active)}, N(eosio.token), N(transfer),
          std::make_tuple(_self, N(eosotcbackup), fee,
                          std::string("buy fee")))
      .send();

      quantity -= fee;
    }
}

void eospixels::onTransfer(const currency::transfer &transfer) {
  if (transfer.to != _self) return;

  if (transfer.memo == "buy") {
    auto t = transfer.quantity;
//    charge_buy_fee(t);

    buy(transfer.from, t); 
    return;
  }

  auto canvasItr = canvases.begin();
  eosio_assert(canvasItr != canvases.end(), "game not started");
  auto canvas = *canvasItr;
  eosio_assert(!canvas.isEnded(), "game ended");

  auto from = transfer.from;
  auto accountItr = accounts.find(from);
  eosio_assert(accountItr != accounts.end(),
               "account not registered to the game");

  pixel_store allPixels(_self, canvas.id);

  auto memo = TransferMemo();
  memo.parse(transfer.memo);

  auto ctx = st_transferContext();
  ctx.amountLeft = transfer.quantity.amount;
  ctx.purchaser = transfer.from;
  ctx.referrer = memo.referrer;

  // Remove referrer if it is invalid
  if (ctx.referrer != 0 &&
      (ctx.referrer == from || !isValidReferrer(ctx.referrer))) {
    ctx.referrer = 0;
  }

  // Every pixel has a "fee". For IPO the fee is the whole pixel price. For
  // takeover, the fee is a percentage of the price increase.

  for (auto &pixelOrder : memo.pixelOrders) {
    drawPixel(allPixels, pixelOrder, ctx);
  }

  size_t paintSuccessPercent =
      ctx.paintedPixelCount * 100 / memo.pixelOrders.size();
  eosio_assert(paintSuccessPercent >= 80, "Too many pixels did not paint.");

  if (ctx.amountLeft > 0) {
    // Refund user with whatever is left over
    deposit(from, ctx.amountLeftScaled());
  }


  

  ctx.updateFeesDistribution();

  canvases.modify(canvasItr, 0, [&](auto &cv) {
    cv.lastPaintedAt = now();
    cv.lastPainter = from;

    ctx.updateCanvas(cv);
  });

  accounts.modify(accountItr, 0,
                  [&](account &acct) { ctx.updatePurchaserAccount(acct); });

  if (ctx.hasReferrer()) {
    if (ctx.referrer != N(knightplay11)) {
      buy(ctx.referrer, asset(ctx.referralEarningScaled / PRECISION_BASE, EOS_SYMBOL));
    }
    //deposit(ctx.referrer, ctx.referralEarningScaled);
  }
}

const double BASE_SELL_FEE_RATIO = 0.20; // 20%
const double ANTI_SELL_FEE_RATIO = 0.30; // 50%
const double START = 1538395200; // 10/01/2018 @ 12:00pm (UTC)
const double END =   1541073600; // 11/01/2018 @ 12:00pm (UTC)

void eospixels::charge_sell_fee(asset& quantity) {
    const double NOW = now();
    double fee_ratio = BASE_SELL_FEE_RATIO;
    if (NOW >= END){
    } else if (NOW <= START) {
        fee_ratio += ANTI_SELL_FEE_RATIO;
    } else {
        fee_ratio = ANTI_SELL_FEE_RATIO * (END-NOW) / (END-START);
    }
    auto fee = asset(quantity.amount * fee_ratio, quantity.symbol);
    // eosio::token::sub_balance(from, fee);
    // eosio::token::add_balance(N(eosotcbackup), fee, from);      

    if (fee.amount > 0) {
      action(permission_level{_self, N(active)}, N(dacincubator), N(transfer),
          std::make_tuple(_self, N(eosotcbackup), fee,
                          std::string("sell fee")))
      .send();

      quantity -= fee;
    }

}


/*
void eospixels::transfer(account_name from, account_name to, asset quantity, std::string memo) {        
    if (from != N(myeosgroupon) && from != N(_self)) charge_sell_fee(from, quantity);

    if (to == _self) {
        sell(from, quantity);
    } else {  
        eosio::transfer(from, to, quantity, memo);
    }
}*/



void eospixels::end() {
  // anyone can create new canvas
  auto itr = canvases.begin();
  eosio_assert(itr != canvases.end(), "no canvas exists");

  auto c = *itr;
  eosio_assert(c.isEnded(), "canvas still has time left");

  // reclaim memory
  canvases.erase(itr);

  // create new canvas
  canvases.emplace(_self, [&](canvas &newCanvas) {
    newCanvas.id = c.id + 1;
    newCanvas.lastPaintedAt = now();
    newCanvas.duration = CANVAS_DURATION;
  });
}

void eospixels::refreshLastPaintedAt() {
  auto itr = canvases.begin();
  eosio_assert(itr != canvases.end(), "no canvas exists");

  canvases.modify(itr, 0,
                  [&](canvas &newCanvas) { newCanvas.lastPaintedAt = now(); });
}

void eospixels::refresh() {
  require_auth(TEAM_ACCOUNT);

  refreshLastPaintedAt();
}

void eospixels::changedur(time duration) {
  require_auth(TEAM_ACCOUNT);

  auto itr = canvases.begin();
  eosio_assert(itr != canvases.end(), "no canvas exists");

  canvases.modify(itr, 0,
                  [&](canvas &newCanvas) { newCanvas.duration = duration; });
}

void eospixels::createacct(const account_name account) {
  require_auth(account);

  auto itr = accounts.find(account);
  eosio_assert(itr == accounts.end(), "account already exist");

  accounts.emplace(account, [&](auto &acct) { acct.owner = account; });
}

void eospixels::init() {
  require_auth(_self);
  // make sure table records is empty
  eosio_assert(canvases.begin() == canvases.end(), "already initialized");

  canvases.emplace(_self, [&](canvas &newCanvas) {
    newCanvas.id = 0;
    newCanvas.lastPaintedAt = now();
    newCanvas.duration = CANVAS_DURATION;
  });
}

void eospixels::init2() {
    require_auth(_self);    

    while (_market.begin() != _market.end()) {
        _market.erase(_market.begin());
    }    

    if (_market.begin() == _market.end()) {
        const uint64_t init_dummy_supply = 20000000ll * 10000ll;
        const uint64_t init_dummy_balance = 20000ll * 10000ll;

        _market.emplace(_self, [&](auto &m) {
            m.supply.amount = init_dummy_supply;
            m.supply.symbol = PXL_SYMBOL;
            m.balance.amount = init_dummy_balance;
            m.balance.symbol = EOS_SYMBOL;
            m.progress = 0;
        });        
    }
}


// void eospixels::createpxrs(uint16_t start, uint16_t end) {
//   require_auth(TEAM_ACCOUNT);

//   auto itr = canvases.begin();
//   eosio_assert(itr != canvases.end(), "no canvas exists");

//   pixel_store pixels(_self, itr->id);
//   for (uint16_t i = start; i < end; i++) {
//     pixels.emplace(
//       _self, [&](pixel_row &pixelRow) { pixelRow.row = i; });
//   }
// }

void eospixels::withdraw(const account_name to) {
  require_auth(to);

  auto canvasItr = canvases.begin();
  eosio_assert(canvasItr != canvases.end(), "no canvas exists");

  auto canvas = *canvasItr;
  /*eosio_assert(canvas.pixelsDrawn >= WITHDRAW_PIXELS_THRESHOLD,
               "canvas still in game initialization");*/

  auto acctItr = accounts.find(to);
  eosio_assert(acctItr != accounts.end(), "unknown account");

  auto guardItr = guards.begin();
  eosio_assert(guardItr != guards.end(), "no withdraw guard exists");

  auto player = *acctItr;
  auto grd = *guardItr;

  uint64_t withdrawAmount = calculateWithdrawalAndUpdate(canvas, player, grd);

  guards.modify(guardItr, 0, [&](guard &g) { g.quota = grd.quota; });

  accounts.modify(acctItr, 0, [&](account &acct) {
    acct.balanceScaled = player.balanceScaled;
    acct.maskScaled = player.maskScaled;
  });

  auto quantity = asset(withdrawAmount, EOS_SYMBOL);
  action(permission_level{_self, N(active)}, N(eosio.token), N(transfer),
         std::make_tuple(_self, to, quantity,
                         std::string("Withdraw from EOS Pixels")))
      .send();
}

void eospixels::deposit(const account_name user,
                        const uint128_t quantityScaled) {
  eosio_assert(quantityScaled > 0, "must deposit positive quantity");

  auto itr = accounts.find(user);

  accounts.modify(itr, 0,
                  [&](auto &acct) { acct.balanceScaled += quantityScaled; });
}

struct account2 {
    asset    balance;
    uint64_t primary_key()const { return balance.symbol.name(); }
};
typedef eosio::multi_index<N(accounts2), account2> accounts2;


const double START_GAME = 1538395200 - 23*60*60; // 10/01/2018 @ 12:00pm (UTC)


void eospixels::apply(account_name contract, action_name act) {

  if (contract == N(dacincubator) && act == N(transfer)) {
    // React to transfer notification.
    // DANGER: All methods MUST check whethe token symbol is acceptable.

    auto transfer = unpack_action_data<currency::transfer>();
    eosio_assert(transfer.quantity.symbol == PXL_SYMBOL,
                 "must sell with PXL token");
    if (transfer.to == _self) {
      auto t = transfer.quantity;
      charge_sell_fee(t);

      sell(transfer.from, t);

    /*auto ctx = st_transferContext();    
      
      auto bonus = asset(0, EOS_SYMBOL);

      const auto& sym = eosio::symbol_type(PXL_SYMBOL).name();
      accounts2 supply_account(N(dacincubator), transfer.from);
      auto my_supply = supply_account.get(sym).balance.amount;    

      auto m = _market.begin();
              const uint64_t init_dummy_supply = 20000000ll * 10000ll;
      auto total_supply = m->supply.amount - init_dummy_supply;

      bonus.amount = ctx.potScaled * my_supply / total_supply / PRECISION_BASE ;
      
      if (bonus.amount > 0) {
        action(permission_level{_self, N(active)}, N(eosio.token), N(transfer),
            std::make_tuple(_self, transfer.from, bonus,
                            std::string("bonus")))
        .send();
        ctx.potScaled -= bonus.amount * PRECISION_BASE;
      }*/
    }
    return;
  }

  if (contract == N(eosio.token) && act == N(transfer)) {
    // React to transfer notification.
    // DANGER: All methods MUST check whethe token symbol is acceptable.

    auto transfer = unpack_action_data<currency::transfer>();
    eosio_assert(transfer.quantity.symbol == EOS_SYMBOL,
                 "must pay with EOS token");

    eosio_assert(now() >= START_GAME,
                 "game will be start after 21:00.");

    onTransfer(transfer);
    return;
  }

  if (contract != _self) return;

  // needed for EOSIO_API macro
  auto &thiscontract = *this;
  switch (act) {
    // first argument is name of CPP class, not contract
    EOSIO_API(eospixels, (init)(init2)(refresh)(changedur)(end)(createacct)(withdraw)(
                             clearpixels)(clearaccts)(clearcanvs)(resetquota))
  };
}

extern "C" {
[[noreturn]] void apply(uint64_t receiver, uint64_t code, uint64_t action) {
  eospixels pixels(receiver);
  pixels.apply(code, action);
  eosio_exit(0);
}
}
