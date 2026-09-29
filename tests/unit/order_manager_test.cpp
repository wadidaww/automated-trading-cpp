#include "futu_trader/execution/order_manager.hpp"

#include <gtest/gtest.h>

#include <string>

#include "futu_trader/api/futu_client.hpp"

using namespace futu_trader;

namespace {
Order makeOrder(const std::string& id, const std::string& key) {
  Order o;
  o.orderId = id;
  o.symbol = "700.HK";
  o.quantity = 1;
  o.limitPriceMinor = 10000;
  o.idempotencyKey = key;
  return o;
}
}  // namespace

TEST(OrderManager, SubmitDedupesOnIdempotencyKey) {
  FutuClient client({});
  ASSERT_TRUE(client.connect());
  OrderManager om(client);
  EXPECT_TRUE(om.submit(makeOrder("o1", "k1")));
  EXPECT_FALSE(om.submit(makeOrder("o1", "k1")));
  EXPECT_EQ(om.state("o1"), OrderState::kSubmitted);
  EXPECT_EQ(om.openOrderCount(), 1U);
  EXPECT_TRUE(om.cancel("o1"));
  EXPECT_EQ(om.state("o1"), OrderState::kCancelled);
  EXPECT_EQ(om.openOrderCount(), 0U);
}

TEST(OrderManager, VenueRejectIsRecordedAndKeyIsReleasedForRetry) {
  // Regression: a rejected placeOrder used to be recorded as kSubmitted and burn the key.
  FutuClient client({});  // never connected, so placeOrder returns ""
  OrderManager om(client);
  EXPECT_FALSE(om.submit(makeOrder("o1", "k1")));
  EXPECT_EQ(om.state("o1"), OrderState::kRejected);
  EXPECT_EQ(om.openOrderCount(), 0U);

  ASSERT_TRUE(client.connect());
  EXPECT_TRUE(om.submit(makeOrder("o1", "k1")));  // retry with the same key now succeeds
  EXPECT_EQ(om.state("o1"), OrderState::kSubmitted);
}

TEST(OrderManager, CancelAndModifyUnknownOrderFail) {
  FutuClient client({});
  ASSERT_TRUE(client.connect());
  OrderManager om(client);
  EXPECT_FALSE(om.cancel("nope"));
  EXPECT_FALSE(om.modify("nope", 1, 1));
}

TEST(OrderManager, ModifyUpdatesOrder) {
  FutuClient client({});
  ASSERT_TRUE(client.connect());
  OrderManager om(client);
  ASSERT_TRUE(om.submit(makeOrder("o1", "k1")));
  EXPECT_TRUE(om.modify("o1", 10100, 3));
}
