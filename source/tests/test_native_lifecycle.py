"""Lifecycle behavior without importing Painter/Designer or controlling UI."""
import ast
import ctypes
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock

MODULE = Path(__file__).parents[1] / "substance3d_chinese_translator" / "__init__.py"


def functions_namespace():
    wanted = {
        "_release_native_delegate", "_uninstall_native_ui",
        "_set_translation_enabled", "initializeSDPlugin",
        "_schedule_update_notification", "_teardown_engine",
    }
    tree = ast.parse(MODULE.read_text(encoding="utf-8"))
    definitions = [node for node in tree.body
                   if isinstance(node, ast.FunctionDef) and node.name in wanted]
    settings = Mock()
    timer = Mock()
    namespace = {
        "ctypes": ctypes, "gc": Mock(), "print": Mock(),
        "_native_delegate": Mock(), "_native_unload_safe": False,
        "_free_native_handle": Mock(return_value=True),
        "QtWidgets": SimpleNamespace(QApplication=SimpleNamespace(instance=lambda: object())),
        "QtCore": SimpleNamespace(QSettings=lambda: settings, QTimer=lambda parent: timer),
        "is_safe": lambda obj: obj is not None,
        "getCppPointer": lambda app: [123],
        "IS_TRANSLATION_ENABLED": True, "IS_CLEANING": False, "IS_APP_QUITTING": False,
        "HOST": "designer", "TRANSLATE_DESIGNER_GRAPH": False,
        "PLUGIN_VERSION": "test",
        "_startup_timer": None, "_tool_action": None, "_label_extractor_dialog": None,
        "_update_notification_timer": None,
        "delete": Mock(), "_notify_update_result": Mock(),
        "_clear_native_shortcuts": Mock(), "_disable_native_engine": Mock(),
        "_destroy_tool_dialog": Mock(),
        "_call_native": Mock(), "_get_main_window": lambda: object(),
        "_load_saved_settings": Mock(), "_register_tool_action": Mock(return_value=object()),
        "_apply_shortcuts": Mock(),
    }
    namespace["_start_native_engine"] = Mock(
        side_effect=lambda: namespace["IS_TRANSLATION_ENABLED"])
    exec(compile(ast.Module(body=definitions, type_ignores=[]), str(MODULE), "exec"), namespace)
    return namespace, settings, timer


class NativeLifecycleTests(unittest.TestCase):
    def test_failed_or_interrupted_uninstall_keeps_dll_loaded(self):
        state, _, _ = functions_namespace()
        dll = state["_native_delegate"]
        dll.sp_delegate_uninstall_ui.return_value = 0
        self.assertFalse(state["_uninstall_native_ui"]())
        self.assertFalse(state["_release_native_delegate"]())
        self.assertIs(state["_native_delegate"], dll)
        state["_free_native_handle"].assert_not_called()
        dll.sp_delegate_uninstall_ui.side_effect = RuntimeError("teardown failed")
        self.assertFalse(state["_uninstall_native_ui"]())
        self.assertFalse(state["_release_native_delegate"]())
        state["_free_native_handle"].assert_not_called()

    def test_successful_uninstall_releases_exactly_once(self):
        state, _, _ = functions_namespace()
        dll = state["_native_delegate"]
        dll.sp_delegate_uninstall_ui.return_value = 1
        self.assertTrue(state["_uninstall_native_ui"]())
        self.assertTrue(state["_release_native_delegate"]())
        self.assertTrue(state["_release_native_delegate"]())
        state["_free_native_handle"].assert_called_once_with(dll)
        self.assertIsNone(state["_native_delegate"])

    def test_designer_startup_does_not_overwrite_saved_preference(self):
        state, settings, timer = functions_namespace()
        state["_schedule_update_notification"] = Mock()
        observed = []
        state["_start_native_engine"].side_effect = lambda: observed.append(state["IS_TRANSLATION_ENABLED"])
        state["initializeSDPlugin"]()
        self.assertEqual(observed, [False])
        settings.setValue.assert_not_called()
        timer.start.assert_called_once_with(3000)
        timer.timeout.connect.call_args[0][0]()
        self.assertTrue(state["IS_TRANSLATION_ENABLED"])
        settings.setValue.assert_not_called()
        state["initializeSDPlugin"]()
        state["_register_tool_action"].assert_called_once()

    def test_manual_toggle_cancels_deferred_designer_startup(self):
        state, settings, timer = functions_namespace()
        state["_startup_timer"] = timer
        state["_set_translation_enabled"](False)
        timer.stop.assert_called_once()
        settings.setValue.assert_called_once_with("substance3d_chinese_translator/enabled", False)

    def test_delayed_notification_is_cancelled_on_teardown(self):
        state, _, timer = functions_namespace()
        state["_schedule_update_notification"](object())
        callback = timer.timeout.connect.call_args[0][0]
        state["_teardown_engine"]()
        timer.stop.assert_called_once()
        state["delete"].assert_called_once_with(timer)
        self.assertIsNone(state["_update_notification_timer"])
        callback()
        state["_notify_update_result"].assert_not_called()


if __name__ == "__main__":
    unittest.main()
