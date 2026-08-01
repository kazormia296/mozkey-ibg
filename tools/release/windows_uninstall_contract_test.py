#!/usr/bin/env python3

from __future__ import annotations

from pathlib import Path
import unittest
import xml.etree.ElementTree as ElementTree


ROOT = Path(__file__).resolve().parents[2]
WXS = ROOT / "src/win32/installer/installer_oss_64bit.wxs"
CUSTOM_ACTION = ROOT / "src/win32/custom_action/custom_action.cc"
WIX_NAMESPACE = {"wix": "http://wixtoolset.org/schemas/v4/wxs"}


class WindowsUninstallContractTest(unittest.TestCase):
    def test_heartbeat_unregister_is_checked_commit_action(self) -> None:
        document = ElementTree.parse(WXS)
        definitions = document.findall(
            ".//wix:CustomAction[@Id='UnregisterGrimodexConsumer']",
            WIX_NAMESPACE,
        )
        self.assertEqual(len(definitions), 1)
        action = definitions[0]
        self.assertEqual(action.get("Execute"), "commit")
        self.assertEqual(action.get("Impersonate"), "yes")
        self.assertEqual(action.get("Return"), "check")

        sequence = document.findall(
            ".//wix:InstallExecuteSequence/"
            "wix:Custom[@Action='UnregisterGrimodexConsumer']",
            WIX_NAMESPACE,
        )
        self.assertEqual(len(sequence), 1)
        condition = sequence[0].get("Condition") or ""
        self.assertIn('REMOVE="ALL"', condition)
        self.assertIn("NOT UPGRADINGPRODUCTCODE", condition)

    def test_exact_heartbeat_owner_quiescence_precedes_unregister(self) -> None:
        source = CUSTOM_ACTION.read_text(encoding="utf-8")
        action_start = source.index(
            "UINT __stdcall UnregisterGrimodexConsumer"
        )
        action_end = source.index(
            "#endif  // GOOGLE_JAPANESE_INPUT_BUILD", action_start
        )
        action = source[action_start:action_end]
        stop_broker = action.index("StopMozcBroker()")
        stop_server = action.index("StopMozcServer()")
        unregister = action.index(
            "UnregisterWindowsDesktopConsumerForAppData"
        )
        self.assertLess(stop_broker, unregister)
        self.assertLess(stop_server, unregister)
        self.assertIn(
            "return ERROR_INSTALL_FAILURE",
            action[min(stop_broker, stop_server):unregister],
        )

    def test_broker_shutdown_is_signaled_and_exact_path_checked(self) -> None:
        source = CUSTOM_ACTION.read_text(encoding="utf-8")
        helper_start = source.index("bool StopMozcBroker()")
        helper_end = source.index("// Retrieves the value", helper_start)
        helper = source[helper_start:helper_end]
        self.assertIn("kGrimodexConsumerBrokerShutdownEvent", helper)
        self.assertIn("NamedEventNotifier", helper)
        self.assertIn("mozc::kMozcBroker", helper)
        self.assertIn("TerminateMozcProcessByImageName", helper)

    def test_quiescence_requires_stable_exact_absence(self) -> None:
        source = CUSTOM_ACTION.read_text(encoding="utf-8")
        stop_server_start = source.index("bool StopMozcServer()")
        stop_server_end = source.index("// Retrieves the value", stop_server_start)
        stop_server = source[stop_server_start:stop_server_end]
        self.assertIn("mozc::kMozcServerName", stop_server)
        self.assertIn("TerminateMozcProcessByImageName", stop_server)
        helper_start = source.index("bool TerminateMozcProcessByImageName")
        helper_end = source.index("bool ShutdownZenzRuntimeProcesses", helper_start)
        helper = source[helper_start:helper_end]
        self.assertGreaterEqual(helper.count("CollectMozcProcessIdsByImageName"), 2)
        self.assertIn("::Sleep(100)", helper)
        self.assertIn("if (process_ids.empty())", helper)
        self.assertIn("GetProcessImagePath(process)", helper)
        self.assertIn("return false", helper)


if __name__ == "__main__":
    unittest.main()
