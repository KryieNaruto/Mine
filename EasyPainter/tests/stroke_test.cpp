#include <gtest/gtest.h>

#include "core/stroke/input_source.h"
#include "core/stroke/predictor.h"

using namespace easypainter::stroke;

TEST(InputSource, BuildsDownMoveUpSequence) {
  auto ev = build_events({{0, 0}, {1, 0}, {2, 0}, {3, 0}});
  ASSERT_EQ(ev.size(), 4u);
  EXPECT_EQ(ev.front().type, InputType::kDown);
  EXPECT_EQ(ev.back().type, InputType::kUp);
  EXPECT_EQ(ev[1].type, InputType::kMove);
  EXPECT_GT(ev[1].time_s, ev[0].time_s);
}

TEST(Predictor, EmptyInputNoCrash) {
  Predictor p;
  auto pts = p.predict({});
  EXPECT_TRUE(pts.empty());
}

TEST(Predictor, ProducesPointsForMove) {
  Predictor p;
  auto pts = p.predict({{InputType::kDown, {0, 0}, 0.0f},
                        {InputType::kMove, {1, 1}, 0.05f},
                        {InputType::kUp, {2, 1}, 0.10f}});
  EXPECT_FALSE(pts.empty());
}

TEST(Predictor, ResetClearsState) {
  Predictor p;
  p.predict({{InputType::kDown, {0, 0}, 0.0f},
             {InputType::kMove, {1, 1}, 0.05f}});
  p.reset();
  auto pts = p.predict({{InputType::kDown, {0, 0}, 0.0f},
                        {InputType::kMove, {1, 1}, 0.05f}});
  EXPECT_FALSE(pts.empty());
}
