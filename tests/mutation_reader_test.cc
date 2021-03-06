/*
 * Copyright (C) 2015 ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */


#include <boost/test/unit_test.hpp>
#include <boost/range/irange.hpp>

#include "core/sleep.hh"
#include "core/do_with.hh"
#include "core/thread.hh"

#include "tests/test-utils.hh"
#include "tests/mutation_assertions.hh"
#include "tests/flat_mutation_reader_assertions.hh"
#include "tests/tmpdir.hh"
#include "tests/sstable_utils.hh"
#include "tests/simple_schema.hh"
#include "tests/test_services.hh"
#include "tests/mutation_source_test.hh"

#include "mutation_reader.hh"
#include "schema_builder.hh"
#include "cell_locking.hh"
#include "sstables/sstables.hh"
#include "database.hh"
#include "partition_slice_builder.hh"

static schema_ptr make_schema() {
    return schema_builder("ks", "cf")
        .with_column("pk", bytes_type, column_kind::partition_key)
        .with_column("v", bytes_type, column_kind::regular_column)
        .build();
}

SEASTAR_TEST_CASE(test_combining_two_readers_with_the_same_row) {
    return seastar::async([] {
        auto s = make_schema();

        mutation m1(s, partition_key::from_single_value(*s, "key1"));
        m1.set_clustered_cell(clustering_key::make_empty(), "v", data_value(bytes("v1")), 1);

        mutation m2(s, partition_key::from_single_value(*s, "key1"));
        m2.set_clustered_cell(clustering_key::make_empty(), "v", data_value(bytes("v2")), 2);

        assert_that(make_combined_reader(s, flat_mutation_reader_from_mutations({m1}), flat_mutation_reader_from_mutations({m2})))
            .produces(m2)
            .produces_end_of_stream();
    });
}

SEASTAR_TEST_CASE(test_combining_two_non_overlapping_readers) {
    return seastar::async([] {
        auto s = make_schema();

        mutation m1(s, partition_key::from_single_value(*s, "keyB"));
        m1.set_clustered_cell(clustering_key::make_empty(), "v", data_value(bytes("v1")), 1);

        mutation m2(s, partition_key::from_single_value(*s, "keyA"));
        m2.set_clustered_cell(clustering_key::make_empty(), "v", data_value(bytes("v2")), 2);

        auto cr = make_combined_reader(s, flat_mutation_reader_from_mutations({m1}), flat_mutation_reader_from_mutations({m2}));
        assert_that(std::move(cr))
            .produces(m2)
            .produces(m1)
            .produces_end_of_stream();
    });
}

SEASTAR_TEST_CASE(test_combining_two_partially_overlapping_readers) {
    return seastar::async([] {
        auto s = make_schema();

        mutation m1(s, partition_key::from_single_value(*s, "keyA"));
        m1.set_clustered_cell(clustering_key::make_empty(), "v", data_value(bytes("v1")), 1);

        mutation m2(s, partition_key::from_single_value(*s, "keyB"));
        m2.set_clustered_cell(clustering_key::make_empty(), "v", data_value(bytes("v2")), 1);

        mutation m3(s, partition_key::from_single_value(*s, "keyC"));
        m3.set_clustered_cell(clustering_key::make_empty(), "v", data_value(bytes("v3")), 1);

        assert_that(make_combined_reader(s, flat_mutation_reader_from_mutations({m1, m2}), flat_mutation_reader_from_mutations({m2, m3})))
            .produces(m1)
            .produces(m2)
            .produces(m3)
            .produces_end_of_stream();
    });
}

SEASTAR_TEST_CASE(test_combining_one_reader_with_many_partitions) {
    return seastar::async([] {
        auto s = make_schema();

        mutation m1(s, partition_key::from_single_value(*s, "keyA"));
        m1.set_clustered_cell(clustering_key::make_empty(), "v", data_value(bytes("v1")), 1);

        mutation m2(s, partition_key::from_single_value(*s, "keyB"));
        m2.set_clustered_cell(clustering_key::make_empty(), "v", data_value(bytes("v2")), 1);

        mutation m3(s, partition_key::from_single_value(*s, "keyC"));
        m3.set_clustered_cell(clustering_key::make_empty(), "v", data_value(bytes("v3")), 1);

        std::vector<flat_mutation_reader> v;
        v.push_back(flat_mutation_reader_from_mutations({m1, m2, m3}));
        assert_that(make_combined_reader(s, std::move(v), streamed_mutation::forwarding::no, mutation_reader::forwarding::no))
            .produces(m1)
            .produces(m2)
            .produces(m3)
            .produces_end_of_stream();
    });
}

static mutation make_mutation_with_key(schema_ptr s, dht::decorated_key dk) {
    mutation m(s, std::move(dk));
    m.set_clustered_cell(clustering_key::make_empty(), "v", data_value(bytes("v1")), 1);
    return m;
}

static mutation make_mutation_with_key(schema_ptr s, const char* key) {
    return make_mutation_with_key(s, dht::global_partitioner().decorate_key(*s, partition_key::from_single_value(*s, bytes(key))));
}

SEASTAR_TEST_CASE(test_filtering) {
    return seastar::async([] {
        auto s = make_schema();

        auto m1 = make_mutation_with_key(s, "key1");
        auto m2 = make_mutation_with_key(s, "key2");
        auto m3 = make_mutation_with_key(s, "key3");
        auto m4 = make_mutation_with_key(s, "key4");

        // All pass
        assert_that(make_filtering_reader(flat_mutation_reader_from_mutations({m1, m2, m3, m4}),
                 [] (const dht::decorated_key& dk) { return true; }))
            .produces(m1)
            .produces(m2)
            .produces(m3)
            .produces(m4)
            .produces_end_of_stream();

        // None pass
        assert_that(make_filtering_reader(flat_mutation_reader_from_mutations({m1, m2, m3, m4}),
                 [] (const dht::decorated_key& dk) { return false; }))
            .produces_end_of_stream();

        // Trim front
        assert_that(make_filtering_reader(flat_mutation_reader_from_mutations({m1, m2, m3, m4}),
                [&] (const dht::decorated_key& dk) { return !dk.key().equal(*s, m1.key()); }))
            .produces(m2)
            .produces(m3)
            .produces(m4)
            .produces_end_of_stream();

        assert_that(make_filtering_reader(flat_mutation_reader_from_mutations({m1, m2, m3, m4}),
            [&] (const dht::decorated_key& dk) { return !dk.key().equal(*s, m1.key()) && !dk.key().equal(*s, m2.key()); }))
            .produces(m3)
            .produces(m4)
            .produces_end_of_stream();

        // Trim back
        assert_that(make_filtering_reader(flat_mutation_reader_from_mutations({m1, m2, m3, m4}),
                 [&] (const dht::decorated_key& dk) { return !dk.key().equal(*s, m4.key()); }))
            .produces(m1)
            .produces(m2)
            .produces(m3)
            .produces_end_of_stream();

        assert_that(make_filtering_reader(flat_mutation_reader_from_mutations({m1, m2, m3, m4}),
                 [&] (const dht::decorated_key& dk) { return !dk.key().equal(*s, m4.key()) && !dk.key().equal(*s, m3.key()); }))
            .produces(m1)
            .produces(m2)
            .produces_end_of_stream();

        // Trim middle
        assert_that(make_filtering_reader(flat_mutation_reader_from_mutations({m1, m2, m3, m4}),
                 [&] (const dht::decorated_key& dk) { return !dk.key().equal(*s, m3.key()); }))
            .produces(m1)
            .produces(m2)
            .produces(m4)
            .produces_end_of_stream();

        assert_that(make_filtering_reader(flat_mutation_reader_from_mutations({m1, m2, m3, m4}),
                 [&] (const dht::decorated_key& dk) { return !dk.key().equal(*s, m2.key()) && !dk.key().equal(*s, m3.key()); }))
            .produces(m1)
            .produces(m4)
            .produces_end_of_stream();
    });
}

SEASTAR_TEST_CASE(test_combining_two_readers_with_one_reader_empty) {
    return seastar::async([] {
        auto s = make_schema();
        mutation m1(s, partition_key::from_single_value(*s, "key1"));
        m1.set_clustered_cell(clustering_key::make_empty(), "v", data_value(bytes("v1")), 1);

        assert_that(make_combined_reader(s, flat_mutation_reader_from_mutations({m1}), make_empty_flat_reader(s)))
            .produces(m1)
            .produces_end_of_stream();
    });
}

SEASTAR_TEST_CASE(test_combining_two_empty_readers) {
    return seastar::async([] {
        auto s = make_schema();
        assert_that(make_combined_reader(s, make_empty_flat_reader(s), make_empty_flat_reader(s)))
            .produces_end_of_stream();
    });
}

SEASTAR_TEST_CASE(test_combining_one_empty_reader) {
    return seastar::async([] {
        std::vector<flat_mutation_reader> v;
        auto s = make_schema();
        v.push_back(make_empty_flat_reader(s));
        assert_that(make_combined_reader(s, std::move(v), streamed_mutation::forwarding::no, mutation_reader::forwarding::no))
            .produces_end_of_stream();
    });
}

std::vector<dht::decorated_key> generate_keys(schema_ptr s, int count) {
    auto keys = boost::copy_range<std::vector<dht::decorated_key>>(
        boost::irange(0, count) | boost::adaptors::transformed([s] (int key) {
            auto pk = partition_key::from_single_value(*s, int32_type->decompose(data_value(key)));
            return dht::global_partitioner().decorate_key(*s, std::move(pk));
        }));
    return std::move(boost::range::sort(keys, dht::decorated_key::less_comparator(s)));
}

std::vector<dht::ring_position> to_ring_positions(const std::vector<dht::decorated_key>& keys) {
    return boost::copy_range<std::vector<dht::ring_position>>(keys | boost::adaptors::transformed([] (const dht::decorated_key& key) {
        return dht::ring_position(key);
    }));
}

SEASTAR_TEST_CASE(test_fast_forwarding_combining_reader) {
    return seastar::async([] {
        auto s = make_schema();

        auto keys = generate_keys(s, 7);
        auto ring = to_ring_positions(keys);

        std::vector<std::vector<mutation>> mutations {
            {
                make_mutation_with_key(s, keys[0]),
                make_mutation_with_key(s, keys[1]),
                make_mutation_with_key(s, keys[2]),
            },
            {
                make_mutation_with_key(s, keys[2]),
                make_mutation_with_key(s, keys[3]),
                make_mutation_with_key(s, keys[4]),
            },
            {
                make_mutation_with_key(s, keys[1]),
                make_mutation_with_key(s, keys[3]),
                make_mutation_with_key(s, keys[5]),
            },
            {
                make_mutation_with_key(s, keys[0]),
                make_mutation_with_key(s, keys[5]),
                make_mutation_with_key(s, keys[6]),
            },
        };

        auto make_reader = [&] (const dht::partition_range& pr) {
            std::vector<flat_mutation_reader> readers;
            boost::range::transform(mutations, std::back_inserter(readers), [&pr] (auto& ms) {
                return flat_mutation_reader_from_mutations({ms}, pr);
            });
            return make_combined_reader(s, std::move(readers));
        };

        auto pr = dht::partition_range::make_open_ended_both_sides();
        assert_that(make_reader(pr))
            .produces(keys[0])
            .produces(keys[1])
            .produces(keys[2])
            .produces(keys[3])
            .produces(keys[4])
            .produces(keys[5])
            .produces(keys[6])
            .produces_end_of_stream();

        pr = dht::partition_range::make(ring[0], ring[0]);
            assert_that(make_reader(pr))
                    .produces(keys[0])
                    .produces_end_of_stream()
                    .fast_forward_to(dht::partition_range::make(ring[1], ring[1]))
                    .produces(keys[1])
                    .produces_end_of_stream()
                    .fast_forward_to(dht::partition_range::make(ring[3], ring[4]))
                    .produces(keys[3])
            .fast_forward_to(dht::partition_range::make({ ring[4], false }, ring[5]))
                    .produces(keys[5])
                    .produces_end_of_stream()
            .fast_forward_to(dht::partition_range::make_starting_with(ring[6]))
                    .produces(keys[6])
                    .produces_end_of_stream();
    });
}

SEASTAR_TEST_CASE(test_sm_fast_forwarding_combining_reader) {
    return seastar::async([] {
        storage_service_for_tests ssft;
        simple_schema s;

        const auto pkeys = s.make_pkeys(4);
        const auto ckeys = s.make_ckeys(4);

        auto make_mutation = [&] (uint32_t n) {
            mutation m(s.schema(), pkeys[n]);

            int i{0};
            s.add_row(m, ckeys[i], sprint("val_%i", i));
            ++i;
            s.add_row(m, ckeys[i], sprint("val_%i", i));
            ++i;
            s.add_row(m, ckeys[i], sprint("val_%i", i));
            ++i;
            s.add_row(m, ckeys[i], sprint("val_%i", i));

            return m;
        };

        std::vector<std::vector<mutation>> readers_mutations{
            {make_mutation(0), make_mutation(1), make_mutation(2), make_mutation(3)},
            {make_mutation(0)},
            {make_mutation(2)},
        };

        std::vector<flat_mutation_reader> readers;
        for (auto& mutations : readers_mutations) {
            readers.emplace_back(flat_mutation_reader_from_mutations(mutations, streamed_mutation::forwarding::yes));
        }

        assert_that(make_combined_reader(s.schema(), std::move(readers), streamed_mutation::forwarding::yes, mutation_reader::forwarding::no))
                .produces_partition_start(pkeys[0])
                .produces_end_of_stream()
                .fast_forward_to(position_range::all_clustered_rows())
                .produces_row_with_key(ckeys[0])
                .next_partition()
                .produces_partition_start(pkeys[1])
                .produces_end_of_stream()
                .fast_forward_to(position_range(position_in_partition::before_key(ckeys[2]), position_in_partition::after_key(ckeys[2])))
                .produces_row_with_key(ckeys[2])
                .produces_end_of_stream()
                .fast_forward_to(position_range(position_in_partition::after_key(ckeys[2]), position_in_partition::after_all_clustered_rows()))
                .produces_row_with_key(ckeys[3])
                .produces_end_of_stream()
                .next_partition()
                .produces_partition_start(pkeys[2])
                .fast_forward_to(position_range::all_clustered_rows())
                .produces_row_with_key(ckeys[0])
                .produces_row_with_key(ckeys[1])
                .produces_row_with_key(ckeys[2])
                .produces_row_with_key(ckeys[3])
                .produces_end_of_stream();
    });
}

struct sst_factory {
    schema_ptr s;
    sstring path;
    unsigned gen;
    int level;

    sst_factory(schema_ptr s, const sstring& path, unsigned gen, int level)
        : s(s)
        , path(path)
        , gen(gen)
        , level(level)
    {}

    sstables::shared_sstable operator()() {
        auto sst = sstables::make_sstable(s, path, gen, sstables::sstable::version_types::la, sstables::sstable::format_types::big);
        sst->set_unshared();

        //TODO set sstable level, to make the test more interesting

        return sst;
    }
};

SEASTAR_TEST_CASE(combined_mutation_reader_test) {
    return seastar::async([] {
        storage_service_for_tests ssft;
        //logging::logger_registry().set_logger_level("database", logging::log_level::trace);

        simple_schema s;

        const auto pkeys = s.make_pkeys(4);
        const auto ckeys = s.make_ckeys(4);

        std::vector<mutation> base_mutations = boost::copy_range<std::vector<mutation>>(
                pkeys | boost::adaptors::transformed([&s](const auto& k) { return mutation(s.schema(), k); }));

        // Data layout:
        //   d[xx]
        // b[xx][xx]c
        // a[x    x]

        int i{0};

        // sstable d
        std::vector<mutation> table_d_mutations;

        i = 1;
        table_d_mutations.emplace_back(base_mutations[i]);
        s.add_row(table_d_mutations.back(), ckeys[i], sprint("val_d_%i", i));

        i = 2;
        table_d_mutations.emplace_back(base_mutations[i]);
        s.add_row(table_d_mutations.back(), ckeys[i], sprint("val_d_%i", i));
        const auto t_static_row = s.add_static_row(table_d_mutations.back(), sprint("%i_static_val", i));

        // sstable b
        std::vector<mutation> table_b_mutations;

        i = 0;
        table_b_mutations.emplace_back(base_mutations[i]);
        s.add_row(table_b_mutations.back(), ckeys[i], sprint("val_b_%i", i));

        i = 1;
        table_b_mutations.emplace_back(base_mutations[i]);
        s.add_row(table_b_mutations.back(), ckeys[i], sprint("val_b_%i", i));

        // sstable c
        std::vector<mutation> table_c_mutations;

        i = 2;
        table_c_mutations.emplace_back(base_mutations[i]);
        const auto t_row = s.add_row(table_c_mutations.back(), ckeys[i], sprint("val_c_%i", i));

        i = 3;
        table_c_mutations.emplace_back(base_mutations[i]);
        s.add_row(table_c_mutations.back(), ckeys[i], sprint("val_c_%i", i));

        // sstable a
        std::vector<mutation> table_a_mutations;

        i = 0;
        table_a_mutations.emplace_back(base_mutations[i]);
        s.add_row(table_a_mutations.back(), ckeys[i], sprint("val_a_%i", i));

        i = 3;
        table_a_mutations.emplace_back(base_mutations[i]);
        s.add_row(table_a_mutations.back(), ckeys[i], sprint("val_a_%i", i));

        auto tmp = make_lw_shared<tmpdir>();

        unsigned gen{0};

        std::vector<sstables::shared_sstable> tables = {
                make_sstable_containing(sst_factory(s.schema(), tmp->path, gen++, 0), table_a_mutations),
                make_sstable_containing(sst_factory(s.schema(), tmp->path, gen++, 1), table_b_mutations),
                make_sstable_containing(sst_factory(s.schema(), tmp->path, gen++, 1), table_c_mutations),
                make_sstable_containing(sst_factory(s.schema(), tmp->path, gen++, 2), table_d_mutations)
        };

        auto cs = sstables::make_compaction_strategy(sstables::compaction_strategy_type::leveled, {});
        auto sstables = make_lw_shared<sstables::sstable_set>(cs.make_sstable_set(s.schema()));

        std::vector<flat_mutation_reader> sstable_mutation_readers;

        for (auto table : tables) {
            sstables->insert(table);

            sstable_mutation_readers.emplace_back(
                table->read_range_rows_flat(
                    s.schema(),
                    query::full_partition_range,
                    s.schema()->full_slice(),
                    seastar::default_priority_class(),
                    no_resource_tracking(),
                    streamed_mutation::forwarding::no,
                    mutation_reader::forwarding::yes));
        }

        auto list_reader = make_combined_reader(s.schema(),
                std::move(sstable_mutation_readers));

        auto incremental_reader = make_local_shard_sstable_reader(
                s.schema(),
                sstables,
                query::full_partition_range,
                s.schema()->full_slice(),
                seastar::default_priority_class(),
                no_resource_tracking(),
                nullptr,
                streamed_mutation::forwarding::no,
                mutation_reader::forwarding::yes);

        // merge c[0] with d[1]
        i = 2;
        auto c_d_merged = mutation(s.schema(), pkeys[i]);
        s.add_row(c_d_merged, ckeys[i], sprint("val_c_%i", i), t_row);
        s.add_static_row(c_d_merged, sprint("%i_static_val", i), t_static_row);

        assert_that(std::move(list_reader))
            .produces(table_a_mutations.front())
            .produces(table_b_mutations[1])
            .produces(c_d_merged)
            .produces(table_a_mutations.back());

        assert_that(std::move(incremental_reader))
            .produces(table_a_mutations.front())
            .produces(table_b_mutations[1])
            .produces(c_d_merged)
            .produces(table_a_mutations.back());
    });
}

static mutation make_mutation_with_key(simple_schema& s, dht::decorated_key dk) {
    static int i{0};

    mutation m(s.schema(), std::move(dk));
    s.add_row(m, s.make_ckey(++i), sprint("val_%i", i));
    return m;
}

class dummy_incremental_selector : public reader_selector {
    std::vector<std::vector<mutation>> _readers_mutations;
    streamed_mutation::forwarding _fwd;
    dht::partition_range _pr;

    const dht::token& position() const {
        return _readers_mutations.back().front().token();
    }
    flat_mutation_reader pop_reader() {
        auto muts = std::move(_readers_mutations.back());
        _readers_mutations.pop_back();
        _selector_position = _readers_mutations.empty() ? dht::ring_position::max() : dht::ring_position::starting_at(position());
        return flat_mutation_reader_from_mutations(std::move(muts), _pr, _fwd);
    }
public:
    // readers_mutations is expected to be sorted on both levels.
    // 1) the inner vector is expected to be sorted by decorated_key.
    // 2) the outer vector is expected to be sorted by the decorated_key
    //  of its first mutation.
    dummy_incremental_selector(schema_ptr s,
            std::vector<std::vector<mutation>> reader_mutations,
            dht::partition_range pr = query::full_partition_range,
            streamed_mutation::forwarding fwd = streamed_mutation::forwarding::no)
        : reader_selector(s, dht::ring_position::min())
        , _readers_mutations(std::move(reader_mutations))
        , _fwd(fwd)
        , _pr(std::move(pr)) {
        // So we can pop the next reader off the back
        boost::reverse(_readers_mutations);
        _selector_position = dht::ring_position::starting_at(position());
    }
    virtual std::vector<flat_mutation_reader> create_new_readers(const dht::token* const t) override {
        if (_readers_mutations.empty()) {
            return {};
        }

        std::vector<flat_mutation_reader> readers;

        if (!t) {
            readers.emplace_back(pop_reader());
            return readers;
        }

        while (!_readers_mutations.empty() && *t >= _selector_position.token()) {
            readers.emplace_back(pop_reader());
        }
        return readers;
    }
    virtual std::vector<flat_mutation_reader> fast_forward_to(const dht::partition_range& pr, db::timeout_clock::time_point timeout) override {
        return create_new_readers(&pr.start()->value().token());
    }
};

SEASTAR_TEST_CASE(reader_selector_gap_between_readers_test) {
    return seastar::async([] {
        storage_service_for_tests ssft;

        simple_schema s;
        auto pkeys = s.make_pkeys(3);

        auto mut1 = make_mutation_with_key(s, pkeys[0]);
        auto mut2a = make_mutation_with_key(s, pkeys[1]);
        auto mut2b = make_mutation_with_key(s, pkeys[1]);
        auto mut3 = make_mutation_with_key(s, pkeys[2]);
        std::vector<std::vector<mutation>> readers_mutations{
            {mut1},
            {mut2a},
            {mut2b},
            {mut3}
        };

        auto reader = make_combined_reader(s.schema(),
                std::make_unique<dummy_incremental_selector>(s.schema(), std::move(readers_mutations)),
                streamed_mutation::forwarding::no,
                mutation_reader::forwarding::no);

        assert_that(std::move(reader))
            .produces_partition(mut1)
            .produces_partition(mut2a + mut2b)
            .produces_partition(mut3)
            .produces_end_of_stream();
    });
}

SEASTAR_TEST_CASE(reader_selector_overlapping_readers_test) {
    return seastar::async([] {
        storage_service_for_tests ssft;

        simple_schema s;
        auto pkeys = s.make_pkeys(3);

        auto mut1 = make_mutation_with_key(s, pkeys[0]);
        auto mut2a = make_mutation_with_key(s, pkeys[1]);
        auto mut2b = make_mutation_with_key(s, pkeys[1]);
        auto mut3a = make_mutation_with_key(s, pkeys[2]);
        auto mut3b = make_mutation_with_key(s, pkeys[2]);
        auto mut3c = make_mutation_with_key(s, pkeys[2]);

        tombstone tomb(100, {});
        mut2b.partition().apply(tomb);

        std::vector<std::vector<mutation>> readers_mutations{
            {mut1, mut2a, mut3a},
            {mut2b, mut3b},
            {mut3c}
        };

        auto reader = make_combined_reader(s.schema(),
                std::make_unique<dummy_incremental_selector>(s.schema(), std::move(readers_mutations)),
                streamed_mutation::forwarding::no,
                mutation_reader::forwarding::no);

        assert_that(std::move(reader))
            .produces_partition(mut1)
            .produces_partition(mut2a + mut2b)
            .produces_partition(mut3a + mut3b + mut3c)
            .produces_end_of_stream();
    });
}

SEASTAR_TEST_CASE(reader_selector_fast_forwarding_test) {
    return seastar::async([] {
        storage_service_for_tests ssft;

        simple_schema s;
        auto pkeys = s.make_pkeys(5);

        auto mut1a = make_mutation_with_key(s, pkeys[0]);
        auto mut1b = make_mutation_with_key(s, pkeys[0]);
        auto mut2a = make_mutation_with_key(s, pkeys[1]);
        auto mut2c = make_mutation_with_key(s, pkeys[1]);
        auto mut3a = make_mutation_with_key(s, pkeys[2]);
        auto mut3d = make_mutation_with_key(s, pkeys[2]);
        auto mut4b = make_mutation_with_key(s, pkeys[3]);
        auto mut5b = make_mutation_with_key(s, pkeys[4]);
        std::vector<std::vector<mutation>> readers_mutations{
            {mut1a, mut2a, mut3a},
            {mut1b, mut4b, mut5b},
            {mut2c},
            {mut3d},
        };

        auto reader = make_combined_reader(s.schema(),
                std::make_unique<dummy_incremental_selector>(s.schema(),
                        std::move(readers_mutations),
                        dht::partition_range::make_ending_with(dht::partition_range::bound(pkeys[1], false))),
                streamed_mutation::forwarding::no,
                mutation_reader::forwarding::yes);

        assert_that(std::move(reader))
            .produces_partition(mut1a + mut1b)
            .produces_end_of_stream()
            .fast_forward_to(dht::partition_range::make(dht::partition_range::bound(pkeys[2], true), dht::partition_range::bound(pkeys[3], true)))
            .produces_partition(mut3a + mut3d)
            .fast_forward_to(dht::partition_range::make_starting_with(dht::partition_range::bound(pkeys[4], true)))
            .produces_partition(mut5b)
            .produces_end_of_stream();
    });
}

static const std::size_t new_reader_base_cost{16 * 1024};

template<typename EventuallySucceedingFunction>
static bool eventually_true(EventuallySucceedingFunction&& f) {
    const unsigned max_attempts = 10;
    unsigned attempts = 0;
    while (true) {
        if (f()) {
            return true;
        }

        if (++attempts < max_attempts) {
            seastar::sleep(std::chrono::milliseconds(1 << attempts)).get0();
        } else {
            return false;
        }
    }

    return false;
}

#define REQUIRE_EVENTUALLY_EQUAL(a, b) BOOST_REQUIRE(eventually_true([&] { return a == b; }))


sstables::shared_sstable create_sstable(simple_schema& sschema, const sstring& path) {
    std::vector<mutation> mutations;
    mutations.reserve(1 << 14);

    for (std::size_t p = 0; p < (1 << 10); ++p) {
        mutation m(sschema.schema(), sschema.make_pkey(p));
        sschema.add_static_row(m, sprint("%i_static_val", p));

        for (std::size_t c = 0; c < (1 << 4); ++c) {
            sschema.add_row(m, sschema.make_ckey(c), sprint("val_%i", c));
        }

        mutations.emplace_back(std::move(m));
        thread::yield();
    }

    return make_sstable_containing([&] {
            return make_lw_shared<sstables::sstable>(sschema.schema(), path, 0, sstables::sstable::version_types::la, sstables::sstable::format_types::big);
        }
        , mutations);
}

static
sstables::shared_sstable create_sstable(schema_ptr s, std::vector<mutation> mutations) {
    static thread_local auto tmp = make_lw_shared<tmpdir>();
    static int gen = 0;
    return make_sstable_containing([&] {
        return make_lw_shared<sstables::sstable>(s, tmp->path, gen++, sstables::sstable::version_types::la, sstables::sstable::format_types::big);
    }, mutations);
}

class tracking_reader : public flat_mutation_reader::impl {
    flat_mutation_reader _reader;
    std::size_t _call_count{0};
    std::size_t _ff_count{0};
public:
    tracking_reader(schema_ptr schema, lw_shared_ptr<sstables::sstable> sst, reader_resource_tracker tracker)
        : impl(schema)
        , _reader(sst->read_range_rows_flat(
                        schema,
                        query::full_partition_range,
                        schema->full_slice(),
                        default_priority_class(),
                        tracker,
                        streamed_mutation::forwarding::no,
                        mutation_reader::forwarding::yes)) {
    }

    virtual future<> fill_buffer(db::timeout_clock::time_point timeout) override {
        ++_call_count;
        return _reader.fill_buffer(timeout).then([this] {
            _end_of_stream = _reader.is_end_of_stream();
            while (!_reader.is_buffer_empty()) {
                push_mutation_fragment(_reader.pop_mutation_fragment());
            }
        });
    }

    virtual void next_partition() override {
        _end_of_stream = false;
        clear_buffer_to_next_partition();
        if (is_buffer_empty()) {
            _reader.next_partition();
        }
    }

    virtual future<> fast_forward_to(const dht::partition_range& pr, db::timeout_clock::time_point timeout) override {
        ++_ff_count;
        // Don't forward this to the underlying reader, it will force us
        // to come up with meaningful partition-ranges which is hard and
        // unecessary for these tests.
        return make_ready_future<>();
    }

    virtual future<> fast_forward_to(position_range, db::timeout_clock::time_point timeout) override {
        throw std::bad_function_call();
    }

    std::size_t call_count() const {
        return _call_count;
    }

    std::size_t ff_count() const {
        return _ff_count;
    }
};

class reader_wrapper {
    flat_mutation_reader _reader;
    tracking_reader* _tracker{nullptr};
    db::timeout_clock::time_point _timeout;
public:
    reader_wrapper(
            reader_concurrency_semaphore& semaphore,
            schema_ptr schema,
            lw_shared_ptr<sstables::sstable> sst,
            db::timeout_clock::time_point timeout = db::no_timeout)
        : _reader(make_empty_flat_reader(schema))
        , _timeout(timeout)
    {
        auto ms = mutation_source([this, sst=std::move(sst)] (schema_ptr schema,
                    const dht::partition_range&,
                    const query::partition_slice&,
                    const io_priority_class&,
                    tracing::trace_state_ptr,
                    streamed_mutation::forwarding,
                    mutation_reader::forwarding,
                    reader_resource_tracker res_tracker) {
            auto tracker_ptr = std::make_unique<tracking_reader>(std::move(schema), std::move(sst), res_tracker);
            _tracker = tracker_ptr.get();
            return flat_mutation_reader(std::move(tracker_ptr));
        });

        _reader = make_restricted_flat_reader(semaphore, std::move(ms), schema);
    }

    reader_wrapper(
            reader_concurrency_semaphore& semaphore,
            schema_ptr schema,
            lw_shared_ptr<sstables::sstable> sst,
            db::timeout_clock::duration timeout_duration)
        : reader_wrapper(semaphore, std::move(schema), std::move(sst), db::timeout_clock::now() + timeout_duration) {
    }

    future<> operator()() {
        while (!_reader.is_buffer_empty()) {
            _reader.pop_mutation_fragment();
        }
        return _reader.fill_buffer(_timeout);
    }

    future<> fast_forward_to(const dht::partition_range& pr) {
        return _reader.fast_forward_to(pr, _timeout);
    }

    std::size_t call_count() const {
        return _tracker ? _tracker->call_count() : 0;
    }

    std::size_t ff_count() const {
        return _tracker ? _tracker->ff_count() : 0;
    }

    bool created() const {
        return bool(_tracker);
    }
};

class dummy_file_impl : public file_impl {
    virtual future<size_t> write_dma(uint64_t pos, const void* buffer, size_t len, const io_priority_class& pc) override {
        return make_ready_future<size_t>(0);
    }

    virtual future<size_t> write_dma(uint64_t pos, std::vector<iovec> iov, const io_priority_class& pc) override {
        return make_ready_future<size_t>(0);
    }

    virtual future<size_t> read_dma(uint64_t pos, void* buffer, size_t len, const io_priority_class& pc) override {
        return make_ready_future<size_t>(0);
    }

    virtual future<size_t> read_dma(uint64_t pos, std::vector<iovec> iov, const io_priority_class& pc) override {
        return make_ready_future<size_t>(0);
    }

    virtual future<> flush(void) override {
        return make_ready_future<>();
    }

    virtual future<struct stat> stat(void) override {
        return make_ready_future<struct stat>();
    }

    virtual future<> truncate(uint64_t length) override {
        return make_ready_future<>();
    }

    virtual future<> discard(uint64_t offset, uint64_t length) override {
        return make_ready_future<>();
    }

    virtual future<> allocate(uint64_t position, uint64_t length) override {
        return make_ready_future<>();
    }

    virtual future<uint64_t> size(void) override {
        return make_ready_future<uint64_t>(0);
    }

    virtual future<> close() override {
        return make_ready_future<>();
    }

    virtual subscription<directory_entry> list_directory(std::function<future<> (directory_entry de)> next) override {
        throw std::bad_function_call();
    }

    virtual future<temporary_buffer<uint8_t>> dma_read_bulk(uint64_t offset, size_t range_size, const io_priority_class& pc) override {
        temporary_buffer<uint8_t> buf(1024);

        memset(buf.get_write(), 0xff, buf.size());

        return make_ready_future<temporary_buffer<uint8_t>>(std::move(buf));
    }
};

SEASTAR_TEST_CASE(reader_restriction_file_tracking) {
    return async([&] {
        reader_concurrency_semaphore semaphore(100, 4 * 1024);
        // Testing the tracker here, no need to have a base cost.
        auto permit = semaphore.wait_admission(0).get0();

        {
            reader_resource_tracker resource_tracker(permit);

            auto tracked_file = resource_tracker.track(
                    file(shared_ptr<file_impl>(make_shared<dummy_file_impl>())));

            BOOST_REQUIRE_EQUAL(4 * 1024, semaphore.available_resources().memory);

            auto buf1 = tracked_file.dma_read_bulk<char>(0, 0).get0();
            BOOST_REQUIRE_EQUAL(3 * 1024, semaphore.available_resources().memory);

            auto buf2 = tracked_file.dma_read_bulk<char>(0, 0).get0();
            BOOST_REQUIRE_EQUAL(2 * 1024, semaphore.available_resources().memory);

            auto buf3 = tracked_file.dma_read_bulk<char>(0, 0).get0();
            BOOST_REQUIRE_EQUAL(1 * 1024, semaphore.available_resources().memory);

            auto buf4 = tracked_file.dma_read_bulk<char>(0, 0).get0();
            BOOST_REQUIRE_EQUAL(0 * 1024, semaphore.available_resources().memory);

            auto buf5 = tracked_file.dma_read_bulk<char>(0, 0).get0();
            BOOST_REQUIRE_EQUAL(-1 * 1024, semaphore.available_resources().memory);

            // Reassing buf1, should still have the same amount of units.
            buf1 = tracked_file.dma_read_bulk<char>(0, 0).get0();
            BOOST_REQUIRE_EQUAL(-1 * 1024, semaphore.available_resources().memory);

            // Move buf1 to the heap, so that we can safely destroy it
            auto buf1_ptr = std::make_unique<temporary_buffer<char>>(std::move(buf1));
            BOOST_REQUIRE_EQUAL(-1 * 1024, semaphore.available_resources().memory);

            buf1_ptr.reset();
            BOOST_REQUIRE_EQUAL(0 * 1024, semaphore.available_resources().memory);

            // Move tracked_file to the heap, so that we can safely destroy it.
            auto tracked_file_ptr = std::make_unique<file>(std::move(tracked_file));
            tracked_file_ptr.reset();

            // Move buf4 to the heap, so that we can safely destroy it
            auto buf4_ptr = std::make_unique<temporary_buffer<char>>(std::move(buf4));
            BOOST_REQUIRE_EQUAL(0 * 1024, semaphore.available_resources().memory);

            // Releasing buffers that overlived the tracked-file they
            // originated from should succeed.
            buf4_ptr.reset();
            BOOST_REQUIRE_EQUAL(1 * 1024, semaphore.available_resources().memory);
        }

        // All units should have been deposited back.
        REQUIRE_EVENTUALLY_EQUAL(4 * 1024, semaphore.available_resources().memory);
    });
}

SEASTAR_TEST_CASE(restricted_reader_reading) {
    return async([&] {
        storage_service_for_tests ssft;
        reader_concurrency_semaphore semaphore(2, new_reader_base_cost);

        {
            simple_schema s;
            auto tmp = make_lw_shared<tmpdir>();
            auto sst = create_sstable(s, tmp->path);

            auto reader1 = reader_wrapper(semaphore, s.schema(), sst);

            reader1().get();

            BOOST_REQUIRE_LE(semaphore.available_resources().count, 1);
            BOOST_REQUIRE_LE(semaphore.available_resources().memory, 0);
            BOOST_REQUIRE_EQUAL(reader1.call_count(), 1);

            auto reader2 = reader_wrapper(semaphore, s.schema(), sst);
            auto read2_fut = reader2();

            // reader2 shouldn't be allowed yet
            BOOST_REQUIRE_EQUAL(reader2.call_count(), 0);
            BOOST_REQUIRE_EQUAL(semaphore.waiters(), 1);

            auto reader3 = reader_wrapper(semaphore, s.schema(), sst);
            auto read3_fut = reader3();

            // reader3 shouldn't be allowed yet
            BOOST_REQUIRE_EQUAL(reader3.call_count(), 0);
            BOOST_REQUIRE_EQUAL(semaphore.waiters(), 2);

            // Move reader1 to the heap, so that we can safely destroy it.
            auto reader1_ptr = std::make_unique<reader_wrapper>(std::move(reader1));
            reader1_ptr.reset();

            // reader1's destruction should've freed up enough memory for
            // reader2 by now.
            REQUIRE_EVENTUALLY_EQUAL(reader2.call_count(), 1);
            read2_fut.get();

            // But reader3 should still not be allowed
            BOOST_REQUIRE_EQUAL(reader3.call_count(), 0);
            BOOST_REQUIRE_EQUAL(semaphore.waiters(), 1);

            // Move reader2 to the heap, so that we can safely destroy it.
            auto reader2_ptr = std::make_unique<reader_wrapper>(std::move(reader2));
            reader2_ptr.reset();

            // Again, reader2's destruction should've freed up enough memory
            // for reader3 by now.
            REQUIRE_EVENTUALLY_EQUAL(reader3.call_count(), 1);
            BOOST_REQUIRE_EQUAL(semaphore.waiters(), 0);
            read3_fut.get();

            {
                BOOST_REQUIRE_LE(semaphore.available_resources().memory, 0);

                // Already allowed readers should not be blocked anymore even if
                // there are no more units available.
                read3_fut = reader3();
                BOOST_REQUIRE_EQUAL(reader3.call_count(), 2);
                read3_fut.get();
            }
        }

        // All units should have been deposited back.
        REQUIRE_EVENTUALLY_EQUAL(new_reader_base_cost, semaphore.available_resources().memory);
    });
}

SEASTAR_TEST_CASE(restricted_reader_timeout) {
    return async([&] {
        storage_service_for_tests ssft;
        reader_concurrency_semaphore semaphore(2, new_reader_base_cost);

        {
            simple_schema s;
            auto tmp = make_lw_shared<tmpdir>();
            auto sst = create_sstable(s, tmp->path);

            auto timeout = std::chrono::duration_cast<db::timeout_clock::time_point::duration>(std::chrono::milliseconds{10});

            auto reader1 = reader_wrapper(semaphore, s.schema(), sst, timeout);
            reader1().get();

            auto reader2 = reader_wrapper(semaphore, s.schema(), sst, timeout);
            auto read2_fut = reader2();

            auto reader3 = reader_wrapper(semaphore, s.schema(), sst, timeout);
            auto read3_fut = reader3();

            BOOST_REQUIRE_EQUAL(semaphore.waiters(), 2);

            seastar::sleep<db::timeout_clock>(std::chrono::milliseconds(40)).get();

            // Altough we have regular BOOST_REQUIREs for this below, if
            // the test goes wrong these futures will be still pending
            // when we leave scope and deleted memory will be accessed.
            // To stop people from trying to debug a failing test just
            // assert here so they know this is really just the test
            // failing and the underlying problem is that the timeout
            // doesn't work.
            assert(read2_fut.failed());
            assert(read3_fut.failed());

            // reader2 should have timed out.
            BOOST_REQUIRE(read2_fut.failed());
            BOOST_REQUIRE_THROW(std::rethrow_exception(read2_fut.get_exception()), semaphore_timed_out);

            // readerk should have timed out.
            BOOST_REQUIRE(read3_fut.failed());
            BOOST_REQUIRE_THROW(std::rethrow_exception(read3_fut.get_exception()), semaphore_timed_out);
       }

        // All units should have been deposited back.
        REQUIRE_EVENTUALLY_EQUAL(new_reader_base_cost, semaphore.available_resources().memory);
    });
}

SEASTAR_TEST_CASE(restricted_reader_max_queue_length) {
    return async([&] {
        storage_service_for_tests ssft;

        struct queue_overloaded_exception {};

        reader_concurrency_semaphore semaphore(2, new_reader_base_cost, 2, [] { return std::make_exception_ptr(queue_overloaded_exception()); });

        {
            simple_schema s;
            auto tmp = make_lw_shared<tmpdir>();
            auto sst = create_sstable(s, tmp->path);

            auto reader1_ptr = std::make_unique<reader_wrapper>(semaphore, s.schema(), sst);
            (*reader1_ptr)().get();

            auto reader2_ptr = std::make_unique<reader_wrapper>(semaphore, s.schema(), sst);
            auto read2_fut = (*reader2_ptr)();

            auto reader3_ptr = std::make_unique<reader_wrapper>(semaphore, s.schema(), sst);
            auto read3_fut = (*reader3_ptr)();

            auto reader4 = reader_wrapper(semaphore, s.schema(), sst);

            BOOST_REQUIRE_EQUAL(semaphore.waiters(), 2);

            // The queue should now be full.
            BOOST_REQUIRE_THROW(reader4().get(), queue_overloaded_exception);

            reader1_ptr.reset();
            read2_fut.get();
            reader2_ptr.reset();
            read3_fut.get();
        }

        REQUIRE_EVENTUALLY_EQUAL(new_reader_base_cost, semaphore.available_resources().memory);
    });
}

SEASTAR_TEST_CASE(restricted_reader_create_reader) {
    return async([&] {
        storage_service_for_tests ssft;
        reader_concurrency_semaphore semaphore(100, new_reader_base_cost);

        {
            simple_schema s;
            auto tmp = make_lw_shared<tmpdir>();
            auto sst = create_sstable(s, tmp->path);

            {
                auto reader = reader_wrapper(semaphore, s.schema(), sst);
                // This fast-forward is stupid, I know but the
                // underlying dummy reader won't care, so it's fine.
                reader.fast_forward_to(query::full_partition_range).get();

                BOOST_REQUIRE(reader.created());
                BOOST_REQUIRE_EQUAL(reader.call_count(), 0);
                BOOST_REQUIRE_EQUAL(reader.ff_count(), 1);
            }

            {
                auto reader = reader_wrapper(semaphore, s.schema(), sst);
                reader().get();

                BOOST_REQUIRE(reader.created());
                BOOST_REQUIRE_EQUAL(reader.call_count(), 1);
                BOOST_REQUIRE_EQUAL(reader.ff_count(), 0);
            }
        }

        REQUIRE_EVENTUALLY_EQUAL(new_reader_base_cost, semaphore.available_resources().memory);
    });
}

static mutation compacted(const mutation& m) {
    auto result = m;
    result.partition().compact_for_compaction(*result.schema(), always_gc, gc_clock::now());
    return result;
}

SEASTAR_TEST_CASE(test_fast_forwarding_combined_reader_is_consistent_with_slicing) {
    return async([&] {
        storage_service_for_tests ssft;
        random_mutation_generator gen(random_mutation_generator::generate_counters::no);
        auto s = gen.schema();

        const int n_readers = 10;
        auto keys = gen.make_partition_keys(3);
        std::vector<mutation> combined;
        std::vector<flat_mutation_reader> readers;
        for (int i = 0; i < n_readers; ++i) {
            std::vector<mutation> muts;
            for (auto&& key : keys) {
                mutation m = compacted(gen());
                muts.push_back(mutation(s, key, std::move(m.partition())));
            }
            if (combined.empty()) {
                combined = muts;
            } else {
                int j = 0;
                for (auto&& m : muts) {
                    combined[j++].apply(m);
                }
            }
            mutation_source ds = create_sstable(s, muts)->as_mutation_source();
            readers.push_back(ds.make_reader(s,
                dht::partition_range::make({keys[0]}, {keys[0]}),
                s->full_slice(), default_priority_class(), nullptr,
                streamed_mutation::forwarding::yes,
                mutation_reader::forwarding::yes));
        }

        flat_mutation_reader rd = make_combined_reader(s, std::move(readers),
            streamed_mutation::forwarding::yes,
            mutation_reader::forwarding::yes);

        std::vector<query::clustering_range> ranges = gen.make_random_ranges(3);

        auto check_next_partition = [&] (const mutation& expected) {
            mutation result(expected.schema(), expected.decorated_key());

            rd.consume_pausable([&](mutation_fragment&& mf) {
                position_in_partition::less_compare less(*s);
                if (!less(mf.position(), position_in_partition_view::before_all_clustered_rows())) {
                    BOOST_FAIL(sprint("Received clustering fragment: %s", mf));
                }
                result.partition().apply(*s, std::move(mf));
                return stop_iteration::no;
            }).get();

            for (auto&& range : ranges) {
                auto prange = position_range(range);
                rd.fast_forward_to(prange).get();
                rd.consume_pausable([&](mutation_fragment&& mf) {
                    if (!mf.relevant_for_range(*s, prange.start())) {
                        BOOST_FAIL(sprint("Received fragment which is not relevant for range: %s, range: %s", mf, prange));
                    }
                    position_in_partition::less_compare less(*s);
                    if (!less(mf.position(), prange.end())) {
                        BOOST_FAIL(sprint("Received fragment is out of range: %s, range: %s", mf, prange));
                    }
                    result.partition().apply(*s, std::move(mf));
                    return stop_iteration::no;
                }).get();
            }

            assert_that(result).is_equal_to(expected, ranges);
        };

        check_next_partition(combined[0]);
        rd.fast_forward_to(dht::partition_range::make_singular(keys[2])).get();
        check_next_partition(combined[2]);
    });
}

SEASTAR_TEST_CASE(test_combined_reader_slicing_with_overlapping_range_tombstones) {
    return async([&] {
        storage_service_for_tests ssft;
        simple_schema ss;
        auto s = ss.schema();

        auto rt1 = ss.make_range_tombstone(ss.make_ckey_range(1, 10));
        auto rt2 = ss.make_range_tombstone(ss.make_ckey_range(1, 5)); // rt1 + rt2 = {[1, 5], (5, 10]}

        mutation m1 = ss.new_mutation(make_local_key(s));
        m1.partition().apply_delete(*s, rt1);
        mutation m2 = m1;
        m2.partition().apply_delete(*s, rt2);
        ss.add_row(m2, ss.make_ckey(4), "v2"); // position after rt2.position() but before rt2.end_position().

        std::vector<flat_mutation_reader> readers;

        mutation_source ds1 = create_sstable(s, {m1})->as_mutation_source();
        mutation_source ds2 = create_sstable(s, {m2})->as_mutation_source();

        // upper bound ends before the row in m2, so that the raw is fetched after next fast forward.
        auto range = ss.make_ckey_range(0, 3);

        {
            auto slice = partition_slice_builder(*s).with_range(range).build();
            readers.push_back(ds1.make_reader(s, query::full_partition_range, slice));
            readers.push_back(ds2.make_reader(s, query::full_partition_range, slice));

            auto rd = make_combined_reader(s, std::move(readers),
                streamed_mutation::forwarding::no, mutation_reader::forwarding::no);

            auto prange = position_range(range);
            mutation result(m1.schema(), m1.decorated_key());

            rd.consume_pausable([&] (mutation_fragment&& mf) {
                if (mf.position().has_clustering_key() && !mf.range().overlaps(*s, prange.start(), prange.end())) {
                    BOOST_FAIL(sprint("Received fragment which is not relevant for the slice: %s, slice: %s", mf, range));
                }
                result.partition().apply(*s, std::move(mf));
                return stop_iteration::no;
            }).get();

            assert_that(result).is_equal_to(m1 + m2, query::clustering_row_ranges({range}));
        }

        // Check fast_forward_to()
        {

            readers.push_back(ds1.make_reader(s, query::full_partition_range, s->full_slice(), default_priority_class(),
                nullptr, streamed_mutation::forwarding::yes));
            readers.push_back(ds2.make_reader(s, query::full_partition_range, s->full_slice(), default_priority_class(),
                nullptr, streamed_mutation::forwarding::yes));

            auto rd = make_combined_reader(s, std::move(readers),
                streamed_mutation::forwarding::yes, mutation_reader::forwarding::no);

            auto prange = position_range(range);
            mutation result(m1.schema(), m1.decorated_key());

            rd.consume_pausable([&](mutation_fragment&& mf) {
                BOOST_REQUIRE(!mf.position().has_clustering_key());
                result.partition().apply(*s, std::move(mf));
                return stop_iteration::no;
            }).get();

            rd.fast_forward_to(prange).get();

            position_in_partition last_pos = position_in_partition::before_all_clustered_rows();
            auto consume_clustered = [&] (mutation_fragment&& mf) {
                position_in_partition::less_compare less(*s);
                if (less(mf.position(), last_pos)) {
                    BOOST_FAIL(sprint("Out of order fragment: %s, last pos: %s", mf, last_pos));
                }
                last_pos = position_in_partition(mf.position());
                result.partition().apply(*s, std::move(mf));
                return stop_iteration::no;
            };

            rd.consume_pausable(consume_clustered).get();
            rd.fast_forward_to(position_range(prange.end(), position_in_partition::after_all_clustered_rows())).get();
            rd.consume_pausable(consume_clustered).get();

            assert_that(result).is_equal_to(m1 + m2);
        }
    });
}

SEASTAR_TEST_CASE(test_combined_mutation_source_is_a_mutation_source) {
    return seastar::async([] {
        // Creates a mutation source which combines N mutation sources with mutation fragments spread
        // among them in a round robin fashion.
        auto make_combined_populator = [] (int n_sources) {
            return [=] (schema_ptr s, const std::vector<mutation>& muts) {
                std::vector<lw_shared_ptr<memtable>> memtables;
                for (int i = 0; i < n_sources; ++i) {
                    memtables.push_back(make_lw_shared<memtable>(s));
                }

                int source_index = 0;
                for (auto&& m : muts) {
                    flat_mutation_reader_from_mutations({m}).consume_pausable([&] (mutation_fragment&& mf) {
                        mutation mf_m(m.schema(), m.decorated_key());
                        mf_m.partition().apply(*s, mf);
                        memtables[source_index++ % memtables.size()]->apply(mf_m);
                        return stop_iteration::no;
                    }).get();
                }

                std::vector<mutation_source> sources;
                for (auto&& mt : memtables) {
                    sources.push_back(mt->as_data_source());
                }
                return make_combined_mutation_source(std::move(sources));
            };
        };
        run_mutation_source_tests(make_combined_populator(1));
        run_mutation_source_tests(make_combined_populator(2));
        run_mutation_source_tests(make_combined_populator(3));
    });
}
