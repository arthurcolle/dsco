import os
import sys
import unittest
from datetime import datetime, timedelta, timezone

sys.path.insert(0, os.path.dirname(os.path.dirname(__file__)))

from trading_ops import (
    MarketSnapshot,
    OrderEvent,
    FillEvent,
    RiskLimits,
    add_market_snapshot,
    adverse_selection_monitor,
    adaptive_order_timeout,
    apply_dead_zone,
    bid_ask_depth_imbalance,
    book_shock_monitor,
    cancel_replace_guardrail,
    classify_spread_regime,
    compliance_report,
    cvar_empirical,
    daily_loss_circuit_breaker,
    duplicate_order_key,
    edge_persistence_filter,
    estimate_queue_position,
    estimate_slippage,
    expected_value_at_fill,
    fee_aware_expected_return,
    halt_safe_mode_state,
    heartbeat_payload,
    idempotency_key,
    latest_mid_price,
    liquidity_score,
    market_loss_hard_stop,
    max_order_frequency_cap,
    no_trade_classifier,
    passive_fill_probability,
    portfolio_stress_test_scenarios,
    realized_vs_expected_slippage,
    risk_policy_report,
    select_stale_orders,
    strategy_cooldown_after_loss_streak,
    strategy_pnl_stop,
    trade_count_throttle,
    trade_reason_code,
    var_gaussian,
    what_if_spread_widening,
)


class TradingOpsTests(unittest.TestCase):
    def test_snapshot_history_and_mid_price(self):
        now = datetime(2026, 4, 10, 15, 0, tzinfo=timezone.utc)
        history = []
        add_market_snapshot(history, MarketSnapshot("KXHIGHNY", now, bid=48, ask=52))
        add_market_snapshot(history, MarketSnapshot("KXHIGHNY", now - timedelta(minutes=5), bid=46, ask=50))

        self.assertEqual(len(history), 2)
        self.assertEqual(latest_mid_price(history), 50.0)
        self.assertEqual(history[-1].market, "KXHIGHNY")

    def test_spread_liquidity_and_slippage_helpers(self):
        snapshot = MarketSnapshot(
            "KXHIGHCHI",
            datetime(2026, 4, 10, 15, 0, tzinfo=timezone.utc),
            bid=49,
            ask=51,
            bid_depth=120,
            ask_depth=100,
            bid_size=35,
            ask_size=28,
            volume=2000,
            open_interest=700,
        )

        self.assertEqual(classify_spread_regime(spread=snapshot.spread, mid_price=snapshot.mid_price), "normal")
        self.assertAlmostEqual(bid_ask_depth_imbalance(120, 100), 20 / 220)
        self.assertGreater(liquidity_score(bid_size=35, ask_size=28, spread=snapshot.spread, volume=2000, open_interest=700), 0)
        self.assertGreater(estimate_slippage(side="buy", quantity=20, book=snapshot), 0)
        self.assertGreater(passive_fill_probability(queue_ahead=10, arrival_rate=4, cancel_rate=1, timeout_seconds=30), 0)
        self.assertLessEqual(estimate_queue_position(queue_ahead=10, our_size=5, depth=100), 1.0)
        self.assertLess(adaptive_order_timeout(spread_regime="tight", liquidity=8.0, volatility=0.5), adaptive_order_timeout(spread_regime="stressed", liquidity=1.0, volatility=3.0))

    def test_trade_reason_and_no_trade_decisions(self):
        self.assertEqual(trade_reason_code(edge=0.0, confidence=0.9, liquidity=3.0, spread_regime="normal"), "NO_EDGE")
        self.assertEqual(trade_reason_code(edge=0.09, confidence=0.2, liquidity=3.0, spread_regime="normal"), "LOW_CONFIDENCE")
        self.assertTrue(no_trade_classifier(edge=0.01, confidence=0.9, liquidity=3.0, spread_regime="normal").tradeable is False)
        self.assertTrue(no_trade_classifier(edge=0.08, confidence=0.9, liquidity=3.0, spread_regime="normal").tradeable)
        self.assertEqual(apply_dead_zone(0.01, threshold=0.02), 0.0)
        self.assertTrue(edge_persistence_filter([0.03, 0.04, -0.01, 0.05], threshold=0.02, confirmations=2)["passed"])

    def test_order_keys_and_stale_selection(self):
        now = datetime(2026, 4, 10, 15, 0, tzinfo=timezone.utc)
        orders = [
            OrderEvent("1", "KXHIGHNY", "buy", 10, 0.53, now - timedelta(seconds=120)),
            OrderEvent("2", "KXHIGHNY", "buy", 10, 0.53, now - timedelta(seconds=10)),
        ]
        stale = select_stale_orders(orders, now=now, max_age_seconds=60)
        self.assertEqual([o.order_id for o in stale], ["1"])
        key_a = duplicate_order_key(strategy="edge", market="KXHIGHNY", side="buy", bucket="50-60", signal_time=now)
        key_b = duplicate_order_key(strategy="edge", market="KXHIGHNY", side="buy", bucket="50-60", signal_time=now)
        key_c = idempotency_key(market="KXHIGHNY", side="buy", quantity=10, price=0.53, business_key="abc")
        self.assertEqual(key_a, key_b)
        self.assertEqual(len(key_c), 64)

    def test_book_shock_and_adverse_selection(self):
        shock = book_shock_monitor(current_spread=0.12, median_spread=0.03, depth_imbalance=0.3)
        self.assertTrue(shock["triggered"])
        fills = [
            FillEvent("KXHIGHNY", "buy", 10, 0.52, realized_slippage=0.03, metadata={"expected_slippage": 0.01}),
            FillEvent("KXHIGHNY", "sell", 8, 0.48, realized_slippage=0.02, metadata={"expected_slippage": 0.01}),
        ]
        self.assertEqual(adverse_selection_monitor(fills, post_fill_moves=[-0.02, -0.01])["adverse_count"], 1)
        report = realized_vs_expected_slippage(fills)
        self.assertAlmostEqual(report["delta"], 0.015, places=3)

    def test_risk_helpers_and_reports(self):
        returns = [-0.05, -0.02, 0.01, 0.03, 0.04]
        self.assertGreater(var_gaussian(returns), 0)
        self.assertGreater(cvar_empirical(returns), 0)
        self.assertAlmostEqual(expected_value_at_fill(edge=0.08, market_price=0.5, slippage=0.02, fees=0.01), -0.45)
        self.assertAlmostEqual(fee_aware_expected_return(edge=0.08, fees=0.01, slippage=0.02), 0.05)
        self.assertTrue(market_loss_hard_stop(realized_pnl=-11, limit=10))
        self.assertTrue(daily_loss_circuit_breaker(realized_pnl=-11, threshold=10))
        self.assertTrue(trade_count_throttle(trades_in_hour=5, limit=5))
        self.assertTrue(strategy_pnl_stop(strategy_pnl=-3, stop_loss=2))
        self.assertTrue(strategy_cooldown_after_loss_streak(loss_streak=3))
        scenarios = portfolio_stress_test_scenarios()
        self.assertGreaterEqual(len(scenarios), 3)
        self.assertLess(what_if_spread_widening(edge=0.08, current_spread=0.02, widened_spread=0.05)["edge_after_widening"], 0.08)

    def test_heartbeat_safe_mode_and_compliance(self):
        now = datetime(2026, 4, 10, 15, 0, tzinfo=timezone.utc)
        state = halt_safe_mode_state(failure_count=4, threshold=3, cooldown_seconds=60, now=now)
        self.assertTrue(state.active)
        self.assertIsNotNone(state.recover_after)
        payload = heartbeat_payload(service="exec", healthy=True, queue_lag_seconds=1.5, latency_ms=22.0)
        self.assertEqual(payload["service"], "exec")
        limits = RiskLimits(max_daily_loss=10, max_market_loss=5, max_trade_count_per_hour=2, max_concentration=0.25, max_drawdown=0.2)
        orders = [
            OrderEvent("1", "KXHIGHNY", "buy", 10, 0.6, now - timedelta(seconds=120)),
            OrderEvent("2", "KXHIGHNY", "sell", 8, 0.55, now - timedelta(seconds=30)),
        ]
        fills = [FillEvent("KXHIGHNY", "buy", 10, 0.6, filled_at=now)]
        risk_report = risk_policy_report(orders=orders, fills=fills, limits=limits, current_equity=900, peak_equity=1000, realized_pnl=-12)
        self.assertTrue(risk_report["daily_loss_breached"])
        compliance = compliance_report(orders=orders, fills=fills, limits=limits, now=now)
        self.assertTrue(compliance["max_trade_count_per_hour_breached"])
        self.assertGreaterEqual(compliance["stale_orders"], 1)


if __name__ == "__main__":
    unittest.main()
