#pragma once
#include "order_book.hpp"
#include "trade.hpp"
#include <cstdint>

namespace nanomatch {

// Order flags — bitfield in Order::flags
inline constexpr std::uint8_t FLAG_IOC       = 0x01; // immediate-or-cancel: no rest
inline constexpr std::uint8_t FLAG_FOK       = 0x02; // fill-or-kill: full fill or none
inline constexpr std::uint8_t FLAG_POST_ONLY = 0x04; // never take liquidity

// ─────────────────────────────────────────────────────────────────────────
// MatchingEngine — CRTP-templated trade sink.
//
// Derived passes itself as Sink. Engine calls sink_->on_trade(t) — fully
// inlined at compile time. No virtual dispatch, no callback pointer.
//
// Default sink = NullSink (counts only). Tests subclass with capture.
// ─────────────────────────────────────────────────────────────────────────

struct NullSink {
    std::uint64_t trade_count = 0;
    void on_trade(const TradeReport&) noexcept { ++trade_count; }
};

template <typename Sink = NullSink>
class MatchingEngineT {
public:
    MatchingEngineT(OrderBook& book, Sink& sink) noexcept
        : book_(book), sink_(sink) {}

    [[gnu::always_inline]]
    void emit(const TradeReport& t) noexcept { sink_.on_trade(t); }

    // ─────────────────────────────────────────────────────────────────────
    // submit_limit_order
    //
    // Returns OrderIdx of resting remainder, or INVALID_IDX if fully filled
    // or rejected. Caller's order_id is preserved.
    // ─────────────────────────────────────────────────────────────────────
    OrderIdx submit_limit_order(
        OrderId    id,
        Side       side,
        PriceTicks price,
        Quantity   qty,
        Timestamp  ts_ns,
        std::uint8_t flags = 0) noexcept
    {
        // Reject duplicates — id_map is source of truth
        if (book_.id_map.find(id) != INVALID_IDX) return INVALID_IDX;

        // Match aggressive side against opposite book side
        Quantity remaining = (side == Side::Buy)
            ? match_buy(id, price, qty, ts_ns)
            : match_sell(id, price, qty, ts_ns);

        // Done — IOC never rests, FOK is all-or-nothing (checked at top)
        if (remaining == 0)                       return INVALID_IDX;
        if (flags & FLAG_IOC)                     return INVALID_IDX;

        // Rest remainder on book
        return rest_order(id, side, price, remaining, qty, ts_ns, flags);
    }

    // ─────────────────────────────────────────────────────────────────────
    // cancel_order — O(1) via id_map + intrusive unlink
    // ─────────────────────────────────────────────────────────────────────
    bool cancel_order(OrderId id) noexcept {
        OrderIdx oidx = book_.id_map.find(id);
        if (oidx == INVALID_IDX) return false;

        Order& o = book_.order_pool[oidx];
        PriceLevel& lvl = book_.level_pool[o.level_idx];

        // Unlink from FIFO
        unlink_order_from_level(oidx, o, lvl);

        // If level now empty, unlink from active-level list + clear price index
        if (lvl.head == INVALID_IDX) {
            BookSide& bs = (o.side == Side::Buy) ? book_.bids : book_.asks;
            unlink_empty_level(o.level_idx, lvl, bs);
        }

        // Release pool slot + id_map entry
        book_.id_map.erase(id);
        book_.order_pool.deallocate(oidx);
        return true;
    }

private:
    // ─────────────────────────────────────────────────────────────────────
    // match_buy — aggressive buy walks asks low→high while buy_price >= ask
    // Returns unfilled qty.
    // ─────────────────────────────────────────────────────────────────────
    Quantity match_buy(OrderId aggressor_id, PriceTicks buy_price,
                       Quantity qty, Timestamp ts_ns) noexcept
    {
        BookSide& asks = book_.asks;
        Quantity remaining = qty;

        while (remaining > 0 && asks.best_level != INVALID_IDX) {
            PriceLevelIdx lidx = asks.best_level;
            PriceLevel& lvl = book_.level_pool[lidx];

            // Price cross check: buy crosses ask iff buy_price >= ask_price
            if (buy_price < lvl.price) break;

            remaining = match_at_level(aggressor_id, Side::Buy, lidx, lvl,
                                       remaining, ts_ns);
            // If level drained, advance best_level
            if (lvl.head == INVALID_IDX) {
                asks.best_level = lvl.next_level;
                if (asks.best_level != INVALID_IDX)
                    book_.level_pool[asks.best_level].prev_level = INVALID_IDX;
                // Clear price_index slot
                std::uint32_t off = asks.tick_to_offset(lvl.price);
                if (off != INVALID_IDX) asks.price_index[off] = INVALID_IDX;
                --asks.active_levels;
                book_.level_pool.deallocate(lidx);
            }
        }
        return remaining;
    }

    // match_sell — aggressive sell walks bids high→low while sell_price <= bid
    Quantity match_sell(OrderId aggressor_id, PriceTicks sell_price,
                        Quantity qty, Timestamp ts_ns) noexcept
    {
        BookSide& bids = book_.bids;
        Quantity remaining = qty;

        while (remaining > 0 && bids.best_level != INVALID_IDX) {
            PriceLevelIdx lidx = bids.best_level;
            PriceLevel& lvl = book_.level_pool[lidx];

            if (sell_price > lvl.price) break;

            remaining = match_at_level(aggressor_id, Side::Sell, lidx, lvl,
                                       remaining, ts_ns);
            if (lvl.head == INVALID_IDX) {
                bids.best_level = lvl.next_level;
                if (bids.best_level != INVALID_IDX)
                    book_.level_pool[bids.best_level].prev_level = INVALID_IDX;
                std::uint32_t off = bids.tick_to_offset(lvl.price);
                if (off != INVALID_IDX) bids.price_index[off] = INVALID_IDX;
                --bids.active_levels;
                book_.level_pool.deallocate(lidx);
            }
        }
        return remaining;
    }

    // ─────────────────────────────────────────────────────────────────────
    // match_at_level — walk FIFO at one price level, emit trades
    // Returns remaining qty after this level processed.
    // ─────────────────────────────────────────────────────────────────────
    Quantity match_at_level(OrderId aggressor_id, Side aggressor_side,
                            PriceLevelIdx lidx, PriceLevel& lvl,
                            Quantity remaining, Timestamp ts_ns) noexcept
    {
        (void)lidx;
        while (remaining > 0 && lvl.head != INVALID_IDX) {
            OrderIdx maker_idx = lvl.head;
            Order& maker = book_.order_pool[maker_idx];

            Quantity fill = (remaining < maker.remaining_qty)
                          ? remaining : maker.remaining_qty;

            // Emit trade
            TradeReport tr{};
            tr.aggressor_id   = aggressor_id;
            tr.resting_id     = maker.id;
            tr.price          = maker.price;   // maker price = fill price
            tr.quantity       = fill;
            tr.aggressor_side = aggressor_side;
            tr.ts_ns          = ts_ns;
            emit(tr);

            // Update qtys
            maker.remaining_qty -= fill;
            lvl.total_quantity  -= fill;
            remaining           -= fill;

            // Maker fully filled? Pop from FIFO, free pool slot
            if (maker.remaining_qty == 0) {
                lvl.head = maker.next;
                if (lvl.head != INVALID_IDX)
                    book_.order_pool[lvl.head].prev = INVALID_IDX;
                else
                    lvl.tail = INVALID_IDX;
                --lvl.order_count;

                book_.id_map.erase(maker.id);
                book_.order_pool.deallocate(maker_idx);
            }
        }
        return remaining;
    }

    // ─────────────────────────────────────────────────────────────────────
    // rest_order — place remainder on book
    // ─────────────────────────────────────────────────────────────────────
    OrderIdx rest_order(OrderId id, Side side, PriceTicks price,
                        Quantity remaining, Quantity original,
                        Timestamp ts_ns, std::uint8_t flags) noexcept
    {
        BookSide& bs = (side == Side::Buy) ? book_.bids : book_.asks;
        std::uint32_t off = bs.tick_to_offset(price);
        if (off == INVALID_IDX) return INVALID_IDX; // out of price window — reject

        // Get or create the price level
        PriceLevelIdx lidx = bs.price_index[off];
        if (lidx == INVALID_IDX) {
            lidx = book_.level_pool.allocate();
            if (lidx == INVALID_IDX) return INVALID_IDX; // pool exhausted
            PriceLevel& nlvl = book_.level_pool[lidx];
            nlvl.price          = price;
            nlvl.total_quantity = 0;
            nlvl.order_count    = 0;
            nlvl.head           = INVALID_IDX;
            nlvl.tail           = INVALID_IDX;
            nlvl.next_level     = INVALID_IDX;
            nlvl.prev_level     = INVALID_IDX;
            bs.price_index[off] = lidx;
            ++bs.active_levels;
            insert_level_sorted(side, lidx, nlvl, bs);
        }

        // Allocate the order
        OrderIdx oidx = book_.order_pool.allocate();
        if (oidx == INVALID_IDX) return INVALID_IDX;
        Order& o = book_.order_pool[oidx];
        o.id             = id;
        o.price          = price;
        o.remaining_qty  = remaining;
        o.original_qty   = original;
        o.next           = INVALID_IDX;
        o.prev           = INVALID_IDX;
        o.level_idx      = lidx;
        o.ts_ns          = ts_ns;
        o.side           = side;
        o.flags          = flags;
        o.participant_id = 0;

        // Append to tail of FIFO (price-time priority)
        PriceLevel& lvl = book_.level_pool[lidx];
        if (lvl.tail == INVALID_IDX) {
            lvl.head = oidx;
            lvl.tail = oidx;
        } else {
            book_.order_pool[lvl.tail].next = oidx;
            o.prev = lvl.tail;
            lvl.tail = oidx;
        }
        lvl.total_quantity += remaining;
        ++lvl.order_count;

        book_.id_map.insert(id, oidx);
        return oidx;
    }

    // ─────────────────────────────────────────────────────────────────────
    // insert_level_sorted — splice new level into sorted active-level list
    // Bids: descending price. Asks: ascending price.
    // ─────────────────────────────────────────────────────────────────────
    void insert_level_sorted(Side side, PriceLevelIdx new_idx,
                             PriceLevel& nlvl, BookSide& bs) noexcept
    {
        // Empty list
        if (bs.best_level == INVALID_IDX) {
            bs.best_level = new_idx;
            return;
        }

        // Find insertion point — walk from best
        PriceLevelIdx cur = bs.best_level;
        PriceLevelIdx prev = INVALID_IDX;
        while (cur != INVALID_IDX) {
            PriceLevel& clvl = book_.level_pool[cur];
            bool better = (side == Side::Buy)
                ? (nlvl.price > clvl.price)   // bids: higher = better
                : (nlvl.price < clvl.price);  // asks: lower  = better
            if (better) break;
            prev = cur;
            cur  = clvl.next_level;
        }

        // Splice
        nlvl.next_level = cur;
        nlvl.prev_level = prev;
        if (cur  != INVALID_IDX) book_.level_pool[cur].prev_level = new_idx;
        if (prev != INVALID_IDX) book_.level_pool[prev].next_level = new_idx;
        else                     bs.best_level = new_idx;
    }

    // Unlink order from its price-level FIFO. O(1) via prev/next.
    void unlink_order_from_level(OrderIdx oidx, Order& o, PriceLevel& lvl) noexcept
    {
        if (o.prev != INVALID_IDX) book_.order_pool[o.prev].next = o.next;
        else                       lvl.head = o.next;
        if (o.next != INVALID_IDX) book_.order_pool[o.next].prev = o.prev;
        else                       lvl.tail = o.prev;
        lvl.total_quantity -= o.remaining_qty;
        --lvl.order_count;
        (void)oidx;
    }

    // Unlink an empty level from sorted active-level list + free it.
    void unlink_empty_level(PriceLevelIdx lidx, PriceLevel& lvl, BookSide& bs) noexcept
    {
        if (lvl.prev_level != INVALID_IDX)
            book_.level_pool[lvl.prev_level].next_level = lvl.next_level;
        else
            bs.best_level = lvl.next_level;
        if (lvl.next_level != INVALID_IDX)
            book_.level_pool[lvl.next_level].prev_level = lvl.prev_level;

        std::uint32_t off = bs.tick_to_offset(lvl.price);
        if (off != INVALID_IDX) bs.price_index[off] = INVALID_IDX;
        --bs.active_levels;
        book_.level_pool.deallocate(lidx);
    }

private:
    OrderBook& book_;
    Sink&      sink_;
};

// Backward-compatible alias — uses NullSink (counts trades only)
using MatchingEngine = MatchingEngineT<NullSink>;

} // namespace nanomatch