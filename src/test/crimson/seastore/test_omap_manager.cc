// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#include "test/crimson/gtest_seastar.h"

#include "test/crimson/seastore/transaction_manager_test_state.h"

#include "crimson/os/seastore/cache.h"
#include "crimson/os/seastore/transaction_manager.h"
#include "crimson/os/seastore/segment_manager.h"
#include "crimson/os/seastore/omap_manager.h"

#include "test/crimson/seastore/test_block.h"

using namespace crimson;
using namespace crimson::os;
using namespace crimson::os::seastore;
using namespace std;

namespace {
  [[maybe_unused]] seastar::logger& logger() {
    return crimson::get_logger(ceph_subsys_test);
  }
}

const int STR_LEN = 50;

std::string rand_name(const int len)
{
  std::string ret;
  ret.reserve(len);
  for (int i = 0; i < len; ++i) {
    ret.append(1, (char)(rand() % ('z' - '0')) + '0');
  }
  return ret;
}

bufferlist rand_buffer(const int len) {
  bufferptr ptr(len);
  for (auto i = ptr.c_str(); i < ptr.c_str() + len; ++i) {
    *i = (char)rand();
  }
  bufferlist bl;
  bl.append(ptr);
  return bl;
}

struct omap_manager_test_t :
  public seastar_test_suite_t,
  TMTestState {

  OMapManagerRef omap_manager;

  omap_manager_test_t() {}

  seastar::future<> set_up_fut() final {
    return tm_setup().then([this] {
      omap_manager = omap_manager::create_omap_manager(*tm);
      return seastar::now();
    });
  }

  seastar::future<> tear_down_fut() final {
    return tm_teardown().then([this] {
      omap_manager.reset();
      return seastar::now();
    });
  }

  using test_omap_t = std::map<std::string, ceph::bufferlist>;
  test_omap_t test_omap_mappings;

  void set_key(
    omap_root_t &omap_root,
    Transaction &t,
    const string &key,
    const bufferlist &val) {
    omap_manager->omap_set_key(omap_root, t, key, val).unsafe_get0();
    test_omap_mappings[key] = val;
  }

  void set_key(
    omap_root_t &omap_root,
    Transaction &t,
    const string &key,
    const string &val) {
    bufferlist bl;
    bl.append(val);
    set_key(omap_root, t, key, bl);
  }

  std::string set_random_key(
    omap_root_t &omap_root,
    Transaction &t) {
    auto key = rand_name(STR_LEN);
    set_key(
      omap_root,
      t,
      key,
      rand_buffer(STR_LEN));
    return key;
  }

  void get_value(
    omap_root_t &omap_root,
    Transaction &t,
    const string &key) {
    auto ret = omap_manager->omap_get_value(omap_root, t, key).unsafe_get0();
    auto iter = test_omap_mappings.find(key);
    if (iter == test_omap_mappings.end()) {
      EXPECT_FALSE(ret);
    } else {
      EXPECT_TRUE(ret);
      if (ret) {
	EXPECT_TRUE(*ret == iter->second);
      }
    }
  }

  void rm_key(
    omap_root_t &omap_root,
    Transaction &t,
    const string &key) {
    omap_manager->omap_rm_key(omap_root, t, key).unsafe_get0();
    test_omap_mappings.erase(test_omap_mappings.find(key));
  }

  void list(
    const omap_root_t &omap_root,
    Transaction &t,
    const std::optional<std::string> &start,
    size_t max = 128) {

    if (start) {
      logger().debug("list on {}", *start);
    } else {
      logger().debug("list on start");
    }

    auto config = OMapManager::omap_list_config_t::with_max(max);
    config.max_result_size = max;
    auto [complete, results] = omap_manager->omap_list(
      omap_root, t, start, config
    ).unsafe_get0();

    auto it = start ?
      test_omap_mappings.upper_bound(*start) :
      test_omap_mappings.begin();
    for (auto &&[k, v]: results) {
      EXPECT_NE(it, test_omap_mappings.end());
      if (it == test_omap_mappings.end())
	return;
      EXPECT_EQ(k, it->first);
      EXPECT_EQ(v, it->second);
      it++;
    }
    if (it == test_omap_mappings.end()) {
      EXPECT_TRUE(complete);
    } else {
      EXPECT_EQ(results.size(), max);
    }
  }

  void clear(
    omap_root_t &omap_root,
    Transaction &t) {
    omap_manager->omap_clear(omap_root, t).unsafe_get0();
    EXPECT_EQ(omap_root.get_location(), L_ADDR_NULL);
  }

  void check_mappings(omap_root_t &omap_root, Transaction &t) {
    for (const auto &i: test_omap_mappings){
      get_value(omap_root, t, i.first);
    }
  }

  void check_mappings(omap_root_t &omap_root) {
    auto t = tm->create_transaction();
    check_mappings(omap_root, *t);
  }

  void replay() {
    logger().debug("{}: begin", __func__);
    restart();
    omap_manager = omap_manager::create_omap_manager(*tm);
    logger().debug("{}: end", __func__);
  }
};

TEST_F(omap_manager_test_t, basic)
{
  run_async([this] {
    omap_root_t omap_root(L_ADDR_NULL, 0);
    {
      auto t = tm->create_transaction();
      omap_root = omap_manager->initialize_omap(*t).unsafe_get0();
      tm->submit_transaction(std::move(t)).unsafe_get();
    }

    string key = "owner";
    string val = "test";

    {
      auto t = tm->create_transaction();
      logger().debug("first transaction");
      set_key(omap_root, *t, key, val);
      get_value(omap_root, *t, key);
      tm->submit_transaction(std::move(t)).unsafe_get();
    }
    {
      auto t = tm->create_transaction();
      logger().debug("second transaction");
      get_value(omap_root, *t, key);
      rm_key(omap_root, *t, key);
      get_value(omap_root, *t, key);
      tm->submit_transaction(std::move(t)).unsafe_get();
    }
    {
      auto t = tm->create_transaction();
      logger().debug("third transaction");
      get_value(omap_root, *t, key);
      tm->submit_transaction(std::move(t)).unsafe_get();
    }
  });
}

TEST_F(omap_manager_test_t, force_leafnode_split)
{
  run_async([this] {
    omap_root_t omap_root(L_ADDR_NULL, 0);
    {
      auto t = tm->create_transaction();
      omap_root = omap_manager->initialize_omap(*t).unsafe_get0();
      tm->submit_transaction(std::move(t)).unsafe_get();
    }
    for (unsigned i = 0; i < 40; i++) {
      auto t = tm->create_transaction();
      logger().debug("opened transaction");
      for (unsigned j = 0; j < 10; ++j) {
        set_random_key(omap_root, *t);
        if ((i % 20 == 0) && (j == 5)) {
          check_mappings(omap_root, *t);
        }
      }
      logger().debug("force split submit transaction i = {}", i);
      tm->submit_transaction(std::move(t)).unsafe_get();
      check_mappings(omap_root);
    }
  });
}

TEST_F(omap_manager_test_t, force_leafnode_split_merge)
{
  run_async([this] {
    omap_root_t omap_root(L_ADDR_NULL, 0);
    {
      auto t = tm->create_transaction();
      omap_root = omap_manager->initialize_omap(*t).unsafe_get0();
      tm->submit_transaction(std::move(t)).unsafe_get();
    }

    for (unsigned i = 0; i < 80; i++) {
      auto t = tm->create_transaction();
      logger().debug("opened split_merge transaction");
      for (unsigned j = 0; j < 5; ++j) {
        set_random_key(omap_root, *t);
        if ((i % 10 == 0) && (j == 3)) {
          check_mappings(omap_root, *t);
        }
      }
      logger().debug("submitting transaction");
      tm->submit_transaction(std::move(t)).unsafe_get();
      if (i % 50 == 0) {
        check_mappings(omap_root);
      }
    }
    auto t = tm->create_transaction();
    int i = 0;
    for (auto &e: test_omap_mappings) {
      if (i % 3 != 0) {
        rm_key(omap_root, *t, e.first);
      }

      if (i % 10 == 0) {
        logger().debug("submitting transaction i= {}", i);
        tm->submit_transaction(std::move(t)).unsafe_get();
        t = tm->create_transaction();
      }
      if (i % 100 == 0) {
        logger().debug("check_mappings  i= {}", i);
        check_mappings(omap_root, *t);
        check_mappings(omap_root);
      }
      i++;
    }
    logger().debug("finally submitting transaction ");
    tm->submit_transaction(std::move(t)).unsafe_get();
  });
}

TEST_F(omap_manager_test_t, force_leafnode_split_merge_fullandbalanced)
{
  run_async([this] {
    omap_root_t omap_root(L_ADDR_NULL, 0);
    {
      auto t = tm->create_transaction();
      omap_root = omap_manager->initialize_omap(*t).unsafe_get0();
      tm->submit_transaction(std::move(t)).unsafe_get();
    }

    for (unsigned i = 0; i < 50; i++) {
      auto t = tm->create_transaction();
      logger().debug("opened split_merge transaction");
      for (unsigned j = 0; j < 5; ++j) {
        set_random_key(omap_root, *t);
        if ((i % 10 == 0) && (j == 3)) {
          check_mappings(omap_root, *t);
        }
      }
      logger().debug("submitting transaction");
      tm->submit_transaction(std::move(t)).unsafe_get();
      if (i % 50 == 0) {
        check_mappings(omap_root);
      }
    }
    auto t = tm->create_transaction();
    int i = 0;
    for (auto &e: test_omap_mappings) {
      if (30 < i && i < 100) {
        auto val = e;
        rm_key(omap_root, *t, e.first);
      }

      if (i % 10 == 0) {
      logger().debug("submitting transaction i= {}", i);
        tm->submit_transaction(std::move(t)).unsafe_get();
        t = tm->create_transaction();
      }
      if (i % 50 == 0) {
      logger().debug("check_mappings  i= {}", i);
        check_mappings(omap_root, *t);
        check_mappings(omap_root);
      }
      i++;
      if (i == 100)
 break;
    }
    logger().debug("finally submitting transaction ");
    tm->submit_transaction(std::move(t)).unsafe_get();
    check_mappings(omap_root);
  });
}

TEST_F(omap_manager_test_t, force_split_listkeys_list_clear)
{
  run_async([this] {
    omap_root_t omap_root(L_ADDR_NULL, 0);
    {
      auto t = tm->create_transaction();
      omap_root = omap_manager->initialize_omap(*t).unsafe_get0();
      tm->submit_transaction(std::move(t)).unsafe_get();
    }
    string temp;
    for (unsigned i = 0; i < 40; i++) {
      auto t = tm->create_transaction();
      logger().debug("opened transaction");
      for (unsigned j = 0; j < 10; ++j) {
        auto key = set_random_key(omap_root, *t);
        if (i == 10)
          temp = key;
        if ((i % 20 == 0) && (j == 5)) {
          check_mappings(omap_root, *t);
        }
      }
      logger().debug("force split submit transaction i = {}", i);
      tm->submit_transaction(std::move(t)).unsafe_get();
      check_mappings(omap_root);
    }

    {
      auto t = tm->create_transaction();
      list(omap_root, *t, std::nullopt);
    }

    {
      auto t = tm->create_transaction();
      list(omap_root, *t, temp, 100);
    }

    {
      auto t = tm->create_transaction();
      clear(omap_root, *t);
      tm->submit_transaction(std::move(t)).unsafe_get();
    }
  });
}

TEST_F(omap_manager_test_t, internal_force_split)
{
  run_async([this] {
    omap_root_t omap_root(L_ADDR_NULL, 0);
    {
      auto t = tm->create_transaction();
      omap_root = omap_manager->initialize_omap(*t).unsafe_get0();
      tm->submit_transaction(std::move(t)).unsafe_get();
    }
    for (unsigned i = 0; i < 10; i++) {
      logger().debug("opened split transaction");
      auto t = tm->create_transaction();

      for (unsigned j = 0; j < 80; ++j) {
        set_random_key(omap_root, *t);
        if ((i % 2 == 0) && (j % 50 == 0)) {
          check_mappings(omap_root, *t);
        }
      }
      logger().debug("submitting transaction i = {}", i);
      tm->submit_transaction(std::move(t)).unsafe_get();
    }
    check_mappings(omap_root);
  });
}

TEST_F(omap_manager_test_t, internal_force_merge_fullandbalanced)
{
  run_async([this] {
    omap_root_t omap_root(L_ADDR_NULL, 0);
    {
      auto t = tm->create_transaction();
      omap_root = omap_manager->initialize_omap(*t).unsafe_get0();
      tm->submit_transaction(std::move(t)).unsafe_get();
    }

    for (unsigned i = 0; i < 8; i++) {
      logger().debug("opened split transaction");
      auto t = tm->create_transaction();

      for (unsigned j = 0; j < 80; ++j) {
        set_random_key(omap_root, *t);
        if ((i % 2 == 0) && (j % 50 == 0)) {
          check_mappings(omap_root, *t);
        }
      }
      logger().debug("submitting transaction");
      tm->submit_transaction(std::move(t)).unsafe_get();
    }
    auto t = tm->create_transaction();
    int i = 0;
    for (auto &e: test_omap_mappings) {
        auto val = e;
        rm_key(omap_root, *t, e.first);

      if (i % 10 == 0) {
      logger().debug("submitting transaction i= {}", i);
        tm->submit_transaction(std::move(t)).unsafe_get();
        t = tm->create_transaction();
      }
      if (i % 50 == 0) {
      logger().debug("check_mappings  i= {}", i);
        check_mappings(omap_root, *t);
        check_mappings(omap_root);
      }
      i++;
    }
    logger().debug("finally submitting transaction ");
    tm->submit_transaction(std::move(t)).unsafe_get();
    check_mappings(omap_root);
  });
}

TEST_F(omap_manager_test_t, replay)
{
  run_async([this] {
    omap_root_t omap_root(L_ADDR_NULL, 0);
    {
      auto t = tm->create_transaction();
      omap_root = omap_manager->initialize_omap(*t).unsafe_get0();
      tm->submit_transaction(std::move(t)).unsafe_get();
      replay();
    }

    for (unsigned i = 0; i < 8; i++) {
      logger().debug("opened split transaction");
      auto t = tm->create_transaction();

      for (unsigned j = 0; j < 80; ++j) {
        set_random_key(omap_root, *t);
        if ((i % 2 == 0) && (j % 50 == 0)) {
          check_mappings(omap_root, *t);
        }
      }
      logger().debug("submitting transaction i = {}", i);
      tm->submit_transaction(std::move(t)).unsafe_get();
    }
    replay();
    check_mappings(omap_root);

    auto t = tm->create_transaction();
    int i = 0;
    for (auto &e: test_omap_mappings) {
        auto val = e;
        rm_key(omap_root, *t, e.first);

      if (i % 10 == 0) {
      logger().debug("submitting transaction i= {}", i);
        tm->submit_transaction(std::move(t)).unsafe_get();
        replay();
        t = tm->create_transaction();
      }
      if (i % 50 == 0) {
      logger().debug("check_mappings  i= {}", i);
        check_mappings(omap_root, *t);
        check_mappings(omap_root);
      }
      i++;
    }
    logger().debug("finally submitting transaction ");
    tm->submit_transaction(std::move(t)).unsafe_get();
    replay();
    check_mappings(omap_root);
  });
}


TEST_F(omap_manager_test_t, internal_force_split_to_root)
{
  run_async([this] {
    omap_root_t omap_root(L_ADDR_NULL, 0);
    {
      auto t = tm->create_transaction();
      omap_root = omap_manager->initialize_omap(*t).unsafe_get0();
      tm->submit_transaction(std::move(t)).unsafe_get();
    }

    logger().debug("set big keys");
    for (unsigned i = 0; i < 53; i++) {
      auto t = tm->create_transaction();

      for (unsigned j = 0; j < 8; ++j) {
        set_random_key(omap_root, *t);
      }
      logger().debug("submitting transaction i = {}", i);
      tm->submit_transaction(std::move(t)).unsafe_get();
    }
     logger().debug("set small keys");
     for (unsigned i = 0; i < 100; i++) {
       auto t = tm->create_transaction();
       for (unsigned j = 0; j < 8; ++j) {
         set_random_key(omap_root, *t);
       }
      logger().debug("submitting transaction last");
      tm->submit_transaction(std::move(t)).unsafe_get();
     }
    check_mappings(omap_root);
  });
}
