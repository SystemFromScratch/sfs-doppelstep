# SPDX-License-Identifier: Apache-2.0
import ast
import os
import threading

from doppelstep_gui import report, scenarios
from doppelstep_gui.device import Bus
from fake_servo import FakeServo

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(
    __file__)))))


def by_id(*ids):
    return [s for s in scenarios.SCENARIOS if s.id in ids]


def run(servo, ids):
    return scenarios.run_scenarios(Bus(servo, timeout=0.05), by_id(*ids), threading.Event(),
                                   log=lambda line: None)


def test_every_scenario_names_an_existing_sil_test():
    for scenario in scenarios.SCENARIOS:
        path, name = scenario.source.split("::")
        tree = ast.parse(open(os.path.join(ROOT, path), encoding="utf-8").read())
        names = {node.name for node in ast.walk(tree) if isinstance(node, ast.FunctionDef)}
        assert name in names, scenario.source


def test_scenario_ids_are_unique():
    ids = [s.id for s in scenarios.SCENARIOS]
    assert len(ids) == len(set(ids))


def test_passing_scenarios():
    servo = FakeServo()
    results = run(servo, ["boot-ping", "boot-eeprom", "motion-mode16"])
    assert [r.status for r in results] == ["PASS"] * 3
    assert servo.reboots == 3                    # fresh start before every scenario


def test_failed_check_stops_the_scenario():
    servo = FakeServo(model=1060)
    [result] = run(servo, ["boot-ping"])
    assert result.status == "FAIL"
    assert "Model Number" in result.error
    assert len([c for c in result.checks if c.ok is not None]) == 1


def test_refused_torque_points_to_the_dead_time():
    [result] = run(FakeServo(torque_fails=True), ["motion-eeprom-lock"])
    assert result.status == "FAIL"
    assert "dead-time-ns" in result.error


def test_eeprom_lock_and_restore_of_the_id():
    servo = FakeServo()
    results = run(servo, ["motion-eeprom-lock", "bus-factory"])
    assert [r.status for r in results] == ["PASS", "PASS"]
    assert servo.id == 1


def test_no_answer_is_an_error_not_a_failure():
    servo = FakeServo()
    servo.silent = True
    [result] = scenarios.run_scenarios(Bus(servo, timeout=0.01), by_id("boot-ping"),
                                       threading.Event(), log=lambda line: None)
    assert result.status == "ERROR"


def test_stop_before_the_start_runs_nothing():
    stop = threading.Event()
    stop.set()
    assert scenarios.run_scenarios(Bus(FakeServo()), scenarios.SCENARIOS, stop,
                                   log=lambda line: None) == []


def test_report_contains_the_results_and_escapes_text():
    results = run(FakeServo(model=1060), ["boot-ping"])
    text = report.build(results, {"Port": "<COM5>"})
    assert "&lt;COM5&gt;" in text
    assert "class='FAIL'" in text
    assert "tests/sil/test_boot.py::test_ping_answers_as_xm430_w350" in text
    assert "test_protection.py" in text           # listed as not repeated
