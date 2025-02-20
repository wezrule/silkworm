/*
   Copyright 2022 The Silkworm Authors

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

#include "header_persistence.hpp"

#include <algorithm>

#include <catch2/catch.hpp>

#include <silkworm/chain/genesis.hpp>
#include <silkworm/common/cast.hpp>
#include <silkworm/common/test_context.hpp>
#include <silkworm/db/genesis.hpp>

#include "header_chain.hpp"

namespace silkworm {

TEST_CASE("header persistence", "[silkworm][downloader][HeaderPersistence]") {
    test::Context context;
    auto& txn{context.txn()};

    bool allow_exceptions = false;

    auto source_data = silkworm::read_genesis_data(silkworm::kMainnetConfig.chain_id);
    auto genesis_json = nlohmann::json::parse(source_data, nullptr, allow_exceptions);
    db::initialize_genesis(txn, genesis_json, allow_exceptions);
    context.commit_txn();

    /* status:
     *         h0
     * input:
     *         h0 <----- h1
     */
    SECTION("one header after the genesis") {
        db::RWTxn tx(context.env());

        auto header0_hash = db::read_canonical_hash(tx, 0);
        REQUIRE(header0_hash.has_value());

        auto header0 = db::read_canonical_header(tx, 0);
        REQUIRE(header0.has_value());

        HeaderPersistence pc(tx);

        REQUIRE(pc.unwind_needed() == false);
        REQUIRE(pc.initial_height() == 0);

        BlockHeader header1;
        header1.number = 1;
        header1.difficulty = 17'171'480'576;
        header1.parent_hash = *header0_hash;
        auto header1_hash = header1.hash();

        auto td = header0->difficulty + header1.difficulty;

        pc.persist(header1);  // here pc write the header on the db

        // check internal status
        REQUIRE(pc.best_header_changed() == true);
        REQUIRE(pc.highest_height() == 1);
        REQUIRE(pc.highest_hash() == header1_hash);
        REQUIRE(pc.total_difficulty() == td);

        // check db content
        REQUIRE(db::read_head_header_hash(tx) == header1_hash);
        REQUIRE(db::read_total_difficulty(tx, 1, header1.hash()) == td);

        auto header1_in_db = db::read_header(tx, header1_hash);
        REQUIRE(header1_in_db.has_value());
        REQUIRE(header1_in_db == header1);

        pc.finish();  // here pc update the canonical chain on the db

        REQUIRE(db::read_canonical_hash(tx, 1) == header1_hash);
    }

    /* status:
     *         h0 (persisted)
     * input:
     *        (h0) <----- h1 <----- h2
     *                |-- h1'
     */
    SECTION("some header after the genesis") {
        db::RWTxn tx(context.env());

        // starting from an initial status
        auto header0 = db::read_canonical_header(tx, 0);
        auto header0_hash = header0->hash();

        // receiving 3 headers from a peer
        BlockHeader header1;
        header1.number = 1;
        header1.difficulty = 1'000'000;
        header1.parent_hash = header0_hash;
        auto header1_hash = header1.hash();

        BlockHeader header2;
        header2.number = 2;
        header2.difficulty = 1'100'000;
        header2.parent_hash = header1_hash;
        auto header2_hash = header2.hash();

        BlockHeader header1b;
        header1b.number = 1;
        header1b.difficulty = 2'000'000;
        header1b.parent_hash = header0_hash;
        header1b.extra_data = string_view_to_byte_view("I'm different");
        auto header1b_hash = header1b.hash();

        // saving the headers
        HeaderPersistence pc(tx);
        pc.persist(header1);
        pc.persist(header2);
        pc.persist(header1b);  // suppose it arrives after header2

        // check internal status
        BigInt expected_td = header0->difficulty + header1.difficulty + header2.difficulty;

        REQUIRE(pc.total_difficulty() == expected_td);
        REQUIRE(pc.best_header_changed() == true);
        REQUIRE(pc.highest_height() == 2);
        REQUIRE(pc.highest_hash() == header2_hash);
        REQUIRE(pc.unwind_needed() == false);

        // check db content
        REQUIRE(db::read_head_header_hash(tx) == header2_hash);
        REQUIRE(db::read_total_difficulty(tx, 2, header2.hash()) == expected_td);

        auto header1_in_db = db::read_header(tx, header1_hash);
        REQUIRE(header1_in_db.has_value());
        REQUIRE(header1_in_db == header1);
        auto header2_in_db = db::read_header(tx, header2_hash);
        REQUIRE(header2_in_db.has_value());
        REQUIRE(header2_in_db == header2);
        auto header1b_in_db = db::read_header(tx, header1b_hash);
        REQUIRE(header1b_in_db.has_value());
        REQUIRE(header1b_in_db == header1b);

        pc.finish();  // here pc update the canonical chain on the db

        REQUIRE(db::read_canonical_hash(tx, 1) == header1_hash);
        REQUIRE(db::read_canonical_hash(tx, 2) == header2_hash);
    }
}

}  // namespace silkworm