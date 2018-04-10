#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>
#include <vector>

#include "poker.h"
#include "util.h"

using namespace std;

// Table contains 1326 masks for all the possible combinations of c1, c2
// where c2 < c1 and c1 = [0..63]
// Index is (c1 << 6) + c2 and highest is 63 * 64 + 63 = 4095
// Indeed because valid cards have (c 0xF) < 13, max index is 3899
uint64_t ScoreMask[4096];

namespace {

const vector<string> Defaults = {
    "2P 3d",
    "3P KhKs - Ac Ad 7c Ts Qs",
    "4P AcTc TdTh - 5h 6h 9c",
    "5P 2c3d KsTc AhTd - 4d 5d 9c 9d",
    "6P Ac Ad KsKd 3c - 2c 2h 7c 7h 8c",
    "7P Ad Kc QhJh 3s4s - 2c 2h 7c 5h 8c",
    "8P - Ac Ah 3d 7h 8c",
    "9P",
    "4P AhAd AcTh 7c6s 2h3h - 2c 3c 4c",
    "4P AhAd AcTh 7c6s 2h3h",
};

// Quick hash, see https://stackoverflow.com/questions/13325125/
// lightweight-8-byte-hash-function-algorithm
struct Hash {

    static const uint64_t Mulp = 2654435789;
    uint64_t mix = 104395301;

    void operator<<(unsigned v) { mix += (v * Mulp) ^ (mix >> 23); }
    uint64_t get() { return mix ^ (mix << 37); }
};

typedef chrono::milliseconds::rep TimePoint; // A value in milliseconds

TimePoint now()
{
    return chrono::duration_cast<chrono::milliseconds>(
        chrono::steady_clock::now().time_since_epoch())
        .count();
}

uint64_t below(uint64_t b) { return (b >> 16) | (b >> 32) | (b >> 48); }
uint64_t to_pick(unsigned n) { return n << 13; }
uint64_t up_to(uint64_t b)
{
    for (int i = 4; i >= 0; --i)
        if (b & RanksBB[i])
            return (b - 1) & RanksBB[i];
    assert(false);
    return 0;
}

class Thread {

    PRNG prng;
    Spot spot;
    size_t gamesNum;
    std::thread* th;
    Result results[PLAYERS_NB];

public:
    Result result(size_t p) const { return results[p]; }

    Thread(size_t idx, const Spot& s, size_t n)
        : prng(idx)
        , spot(s)
        , gamesNum(n)
    {

        memset(results, 0, sizeof(results));
        spot.set_prng(&prng);
        th = new std::thread(&Thread::run, this);
    }

    void join()
    {
        th->join();
        delete th;
    }

    void run()
    {
        for (size_t i = 0; i < gamesNum; i++)
            spot.run(results);
    }
};

} // namespace

void run(const Spot& s, size_t gamesNum, size_t threadsNum, Result results[])
{
    std::vector<Thread*> threads;

    size_t n = gamesNum < threadsNum ? 1 : gamesNum / threadsNum;

    for (size_t i = 0; i < threadsNum; ++i)
        threads.push_back(new Thread(i, s, n));

    for (Thread* th : threads) {
        th->join();
        for (size_t p = 0; p < s.players(); p++) {
            results[p].first += th->result(p).first;
            results[p].second += th->result(p).second;
        }
        delete th;
    }
}

void init_score_mask()
{
    const uint64_t Fixed = FullHouseBB | DoublePairBB | to_pick(7);

    for (unsigned c1 = 0; c1 < 64; c1++) {

        if ((c1 & 0xF) >= INVALID)
            continue;

        for (unsigned c2 = 0; c2 < c1; c2++) {

            if ((c2 & 0xF) >= INVALID)
                continue;

            unsigned idx = (c1 << 6) + c2;

            uint64_t h = 1ULL << c1;
            uint64_t l = 1ULL << c2;

            // High card
            if (h & Rank1BB)
                ScoreMask[idx] = ~Fixed | to_pick(5);

            // Pair
            else if ((h & Rank2BB) && (l & Rank1BB))
                ScoreMask[idx] = ~(Fixed | below(h)) | to_pick(3);

            // Double Pair (there could be also a third one that is dropped)
            else if ((h & Rank2BB) && (l & Rank2BB))
                ScoreMask[idx] = ~(Fixed | below(h) | below(l) | up_to(l)) | DoublePairBB | to_pick(1);

            // Set
            else if ((h & Rank3BB) && (l & Rank1BB))
                ScoreMask[idx] = ~(Fixed | below(h)) | to_pick(2);

            // Full house (there could be also a second pair that is dropped)
            else if ((h & Rank3BB) && (l & Rank2BB)) {
                ScoreMask[idx] = ~(Fixed | below(h) | below(l) | up_to(l)) | FullHouseBB | to_pick(0);
                ScoreMask[idx] &= ~Rank1BB; // Drop all first line
            }
            // Double set: it's a full house, second set is counted as a pair
            else if ((h & Rank3BB) && (l & Rank3BB)) {
                ScoreMask[idx] = ~(Fixed | below(h) | below(l) | up_to(h));
                ScoreMask[idx] |= (l >> 16) | FullHouseBB | to_pick(0);
                ScoreMask[idx] &= ~Rank1BB; // Drop all first line
            }
            // Quad: drop anything else but first rank
            else if ((h & Rank4BB))
                ScoreMask[idx] = ~(Fixed | below(h) | up_to(h) | Rank3BB | Rank2BB) | to_pick(1);
            else
                assert(false);
        }
    }
}

const string pretty_hand(uint64_t b, bool headers)
{
    string s = "\n";
    string hstr = headers ? "\n" : "---+---+---+\n";

    if (headers)
        s += "    | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | T | J | Q | K | A \n";

    s += "    +---+---+---+---+---+---+---+---+---+---+---+---+---+" + hstr;

    for (int r = 3; r >= 0; --r) {
        s += headers ? string("   ") + "dhcs"[r] : string("    ");

        for (int f = 0; f < (headers ? 13 : 16); ++f)
            s += b & (1ULL << ((r * 16) + f)) ? "| X " : "|   ";

        s += "|\n    +---+---+---+---+---+---+---+---+---+---+---+---+---+" + hstr;
    }

    return s;
}

ostream& operator<<(ostream& os, Card c)
{
    if (c % 16 < INVALID)
        os << "23456789TJQKA"[c % 16] << "dhcs"[c / 16] << " ";
    else
        os << "-- ";
    return os;
}

ostream& operator<<(ostream& os, const Hand& h)
{
    vector<Card> cards;
    uint64_t v = h.cards;

    while (v)
        cards.push_back(Card(pop_lsb(&v)));

    // Sort the cards in descending value
    auto comp = [](Card a, Card b) { return (a & 0xF) > (b & 0xF); };
    sort(cards.begin(), cards.end(), comp);

    os << "\n\nHand: ";
    for (Card c : cards)
        os << c;

    os << "\n"
       << pretty_hand(h.cards, true) << "\n";

    if (h.score)
        os << "\nScore:\n"
           << pretty_hand(h.score, false) << "\n";

    return os;
}

void print_results(Result* results, size_t players, size_t games)
{
    cout << std::showpoint << std::noshowpos << std::fixed << std::setprecision(2)
         << "\n     Equity    Win     Tie   Pots won  Pots tied\n";

    for (size_t p = 0; p < players; p++) {
        cout << "P" << p+1 << ": ";
        size_t v = KTie * results[p].first + results[p].second;
        cout << std::setw(6) << v * 100.0 / KTie / games << "% "
             << std::setw(6) << results[p].first * 100.0 / games << "% "
             << std::setw(6) << results[p].second * 100.0 / KTie / games << "% "
             << std::setw(9) << results[p].first << " "
             << std::setw(9) << double(results[p].second) / KTie << endl;
    }
}

void bench(istringstream& is)
{
    constexpr uint64_t GoodSig = 11714201772365687243ULL;
    constexpr int GamesNum = 1500 * 1000;

    Result results[PLAYERS_NB];
    string token;
    uint64_t cards = 0, spots = 0, cnt = 1;
    Hash sig;

    int threadsNum = (is >> token) ? stoi(token) : 1;

    TimePoint elapsed = now();

    for (const string& v : Defaults) {

        cerr << "\nPosition " << cnt++ << ": " << v << endl;
        memset(results, 0, sizeof(results));
        Spot s(v);
        run(s, GamesNum, threadsNum, results);

        for (size_t p = 0; p < s.players(); p++)
            sig << results[p].first + results[p].second;

        print_results(results, s.players(), GamesNum);

        cards += GamesNum * (s.players() * 2 + 5);
        spots += GamesNum;
    }

    elapsed = now() - elapsed + 1; // Ensure positivity to avoid a 'divide by zero'

    cerr << "\n==========================="
         << "\nTotal time  (ms): " << elapsed
         << "\nSpots played (M): " << spots / 1000000
         << "\nCards/second    : " << 1000 * cards / elapsed
         << "\nSpots/second    : " << 1000 * spots / elapsed
         << "\nSignature       : " << sig.get();

    if (sig.get() == GoodSig)
        cerr << " (OK)" << endl;
    else
        cerr << " (FAIL)" << endl;
}
