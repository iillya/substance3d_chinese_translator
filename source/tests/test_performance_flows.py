"""Behavior tests for batching, responsive I/O and the native extractor."""
import ast
import json
import os
import subprocess
import tempfile
import threading
import time
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock


SOURCE = Path(__file__).parents[1]
MODULE = SOURCE / "substance3d_chinese_translator" / "__init__.py"
EXTRACTOR = Path(os.environ.get(
    "SP_TEST_EXTRACTOR", SOURCE / "cpp/build-display/Release/translator_extractor.exe"
))


def load_functions(names, namespace):
    tree = ast.parse(MODULE.read_text(encoding="utf-8"))
    nodes = [node for node in tree.body
             if isinstance(node, ast.FunctionDef) and node.name in names]
    exec(compile(ast.Module(body=nodes, type_ignores=[]), str(MODULE), "exec"), namespace)
    return namespace


class PerformanceFlowTests(unittest.TestCase):
    def test_dictionary_sync_uses_one_ordered_atomic_call_and_keeps_graph_switch(self):
        dll = Mock()
        dll.sp_delegate_replace_translations.return_value = 1
        entries = {"roughglass": "first", "Rough Glass": "last", "Concrete": "混凝土"}
        ns = load_functions({"_sync_native_dictionary"}, {
            "json": json, "os": os, "print": Mock(),
            "_load_native_delegate": lambda: dll,
            "_apply_dictionary_reload_callback": Mock(),
            "HOST": "designer", "TRANSLATIONS_DIR": "translations",
            "TRANSLATE_LAYERS_PANEL": True, "FUZZY_MATCH_ENABLED": True,
            "FALLBACK_SCAN_ENABLED": False, "EDIT_TRIGGER": Mock(),
            "TRANSLATE_DICT": entries, "ID_TRANSLATE_DICTS": {"id": "译文"},
            "IS_TRANSLATION_ENABLED": True,
        })
        self.assertTrue(ns["_sync_native_dictionary"]())
        payload, size = dll.sp_delegate_replace_translations.call_args.args
        self.assertEqual(len(payload), size)
        self.assertEqual(json.loads(payload)["translations"], [list(p) for p in entries.items()])
        dll.sp_delegate_replace_translations.assert_called_once()
        dll.sp_delegate_add_translation.assert_not_called()
        dll.sp_delegate_clear_translations.assert_not_called()
        dll.sp_delegate_set_translate_designer_graph.assert_not_called()
        dll.sp_delegate_set_enabled.reset_mock()
        dll.sp_delegate_replace_translations.return_value = 0
        self.assertFalse(ns["_sync_native_dictionary"]())
        dll.sp_delegate_set_enabled.assert_not_called()

    def query_namespace(self, operation, cancel=False):
        class Cancelled(Exception):
            pass

        callback = {}
        accepted = threading.Event()
        ticks = []
        dialog = Mock()
        dialog.is_cancelled.return_value = cancel
        dialog.accept.side_effect = accepted.set
        timer = Mock()
        timer.timeout.connect.side_effect = lambda fn: callback.update(poll=fn)

        def event_loop():
            deadline = time.monotonic() + 2
            while not accepted.is_set() and time.monotonic() < deadline:
                ticks.append(threading.get_ident())
                callback["poll"]()
                time.sleep(0.001)
            self.assertTrue(accepted.is_set(), "completion must return to the UI event loop")

        dialog.exec_.side_effect = event_loop
        ns = load_functions({"_run_release_query", "_check_updates"}, {
            "threading": threading, "_DownloadCancelled": Cancelled,
            "_DownloadProgressDialog": lambda parent: dialog,
            "QtCore": SimpleNamespace(QTimer=lambda parent: timer),
            "_latest_release_info": operation,
            "IS_CLEANING": False, "IS_APP_QUITTING": False,
            "is_safe": lambda obj: obj is not None, "delete": Mock(),
            "_update_check_active": False, "_check_updates_once": Mock(),
        })
        return ns, timer, dialog, ticks, Cancelled

    def test_release_query_runs_in_worker_while_ui_keeps_processing(self):
        worker_ids = []

        def operation():
            worker_ids.append(threading.get_ident())
            time.sleep(0.03)
            return ("1.3.8", "url", "notes", "hash")

        ns, timer, dialog, ticks, _ = self.query_namespace(operation)
        self.assertEqual(ns["_run_release_query"](None)[0], "1.3.8")
        self.assertNotEqual(worker_ids, [threading.get_ident()])
        self.assertGreater(len(ticks), 1)
        timer.stop.assert_called_once()
        ns["delete"].assert_called_once_with(dialog)

    def test_cancel_does_not_wait_for_slow_network_or_touch_deleted_ui(self):
        release = threading.Event()
        finished = threading.Event()

        def operation():
            release.wait(2)
            finished.set()
            return ("ignored",)

        ns, timer, dialog, _, cancelled = self.query_namespace(operation, cancel=True)
        try:
            with self.assertRaises(cancelled):
                ns["_run_release_query"](None)
            self.assertFalse(finished.is_set(), "UI returned before the network worker")
            timer.stop.assert_called_once()
            calls = list(dialog.mock_calls)
        finally:
            release.set()
        self.assertTrue(finished.wait(1))
        self.assertEqual(dialog.mock_calls, calls, "completed worker never calls Qt")

    def test_query_error_and_shutdown_cleanup_and_reentry_guard(self):
        def fail():
            raise OSError("offline")
        ns, timer, _, _, cancelled = self.query_namespace(fail)
        with self.assertRaisesRegex(OSError, "offline"):
            ns["_run_release_query"](None)
        timer.stop.assert_called_once()
        ns["IS_CLEANING"] = True
        with self.assertRaises(cancelled):
            ns["_run_release_query"](None)
        ns["IS_CLEANING"] = False
        ns["_check_updates_once"].side_effect = lambda parent: ns["_check_updates"](parent)
        ns["_check_updates"](None)
        ns["_check_updates_once"].assert_called_once()
        self.assertFalse(ns["_update_check_active"])


@unittest.skipUnless(EXTRACTOR.is_file(), "build the native extractor to test metadata parsing")
class ExtractorPerformanceTests(unittest.TestCase):
    def extract(self, contents):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            inputs = root / "assets"
            inputs.mkdir()
            for name, content in contents.items():
                (inputs / name).write_text(content, encoding="utf-8")
            output = root / "result_zh.json"
            request = root / "request.json"
            request.write_text(json.dumps({
                "source": str(inputs), "output": str(output),
                "ordinary_filenames": False, "attributes": ["label", "description"],
            }), encoding="utf-8")
            result = subprocess.run([str(EXTRACTOR), "--request", str(request)],
                                    capture_output=True, timeout=8)
            self.assertEqual(result.returncode, 0, result.stdout.decode("utf-8", errors="replace"))
            return json.loads(output.read_text(encoding="utf-8"))["translations"]

    def test_labels_nested_json_and_escaped_braces(self):
        terms = self.extract({"test.glsl": '''// metadata
{"label":"Concrete","nested":{"label12":"Rough Glass"},"description":"Brace { caption }"}
{"label":"Quote \\"caption\\""}
void main() { vec4 color; }
'''})
        for term in ("Concrete", "Rough Glass", "Brace { caption }", 'Quote "caption"'):
            self.assertIn(term, terms)

    def test_many_unmatched_braces_recover_later_metadata_without_quadratic_scan(self):
        terms = self.extract({"broken.glsl": "{" * 100000 + '\n{"label":"Recovered caption"}'})
        self.assertIn("Recovered caption", terms)


if __name__ == "__main__":
    unittest.main()
