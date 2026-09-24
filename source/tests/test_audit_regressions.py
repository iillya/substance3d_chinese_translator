"""Data preservation and process lifecycle regressions for the shared SP/SD code."""
import ast
import json
import os
import shutil
import subprocess
import tempfile
import unittest
import zipfile
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

from test_performance_flows import MODULE, EXTRACTOR, load_functions
from test_security_regressions import _load_build_module, _load_update_validation_namespace


class AuditRegressionTests(unittest.TestCase):
    def update_namespace(self, root):
        plugin = root / "plugin"
        plugin.mkdir()
        (plugin / "__init__.py").write_text("current version", encoding="utf-8")
        (plugin / "personal.txt").write_text("keep", encoding="utf-8")
        backup = root / "backup"
        backup.mkdir()
        (backup / "__init__.py").write_text("unrelated older backup", encoding="utf-8")
        ns = _load_update_validation_namespace()
        ns.update({
            "shutil": shutil, "tempfile": tempfile, "WAIT_CURSOR": 0,
            "PLUGIN_DIR": str(plugin), "UPDATE_BACKUP_DIR": str(backup),
            "UPDATE_RESULT_FILE": str(root / "result.txt"), "HOST_DISPLAY_NAME": "test host",
            "QtWidgets": SimpleNamespace(QApplication=Mock(), QMessageBox=Mock()),
        })
        return load_functions({"_apply_update_now", "_copy_file_safely", "_copytree_merge",
                               "_cleanup_update_remnants", "_load_existing_translations"}, ns)

    def package(self, root, ns):
        path = root / "update.zip"
        with zipfile.ZipFile(path, "w") as archive:
            for name in ns["RELEASE_FILE_ALLOWLIST"]:
                content = "new version"
                if name == "pluginInfo.json":
                    content = json.dumps({"name": "substance3d_chinese_translator", "version": "1.3.7"})
                archive.writestr(name, content)
        return str(path)

    def test_rejected_package_does_not_restore_previous_backup(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            ns = self.update_namespace(root)
            bad = root / "bad.zip"
            bad.write_bytes(b"not a zip")
            self.assertFalse(ns["_apply_update_now"](str(bad)))
            self.assertEqual((root / "plugin/__init__.py").read_text(), "current version")
            self.assertEqual((root / "plugin/personal.txt").read_text(), "keep")
            self.assertEqual((root / "backup/__init__.py").read_text(), "unrelated older backup")

    def test_partial_backup_failure_leaves_installation_untouched(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            ns = self.update_namespace(root)
            package = self.package(root, ns)
            copytree = shutil.copytree

            def fail_backup(source, destination, *args, **kwargs):
                if Path(destination) == root / "backup":
                    Path(destination).mkdir()
                    (Path(destination) / "__init__.py").write_text("partial snapshot")
                    raise OSError("backup disk full")
                return copytree(source, destination, *args, **kwargs)

            with patch.object(shutil, "copytree", side_effect=fail_backup):
                self.assertFalse(ns["_apply_update_now"](package))
            self.assertEqual((root / "plugin/__init__.py").read_text(), "current version")
            self.assertEqual((root / "plugin/personal.txt").read_text(), "keep")

    def test_failed_replacement_rolls_back_fresh_complete_snapshot(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            ns = self.update_namespace(root)
            package = self.package(root, ns)
            copy = ns["_copy_file_safely"]
            failed = []

            def fail_once(source, target):
                if not failed and str(source).endswith("__init__.py"):
                    failed.append(True)
                    copy(source, target)
                    raise OSError("simulated write failure")
                return copy(source, target)

            ns["_copy_file_safely"] = fail_once
            self.assertFalse(ns["_apply_update_now"](package))
            self.assertTrue(failed)
            self.assertEqual((root / "plugin/__init__.py").read_text(), "current version")
            self.assertEqual((root / "plugin/personal.txt").read_text(), "keep")

    def test_cleanup_does_not_remove_another_hosts_staging_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            ns = self.update_namespace(root)
            other = root / "sp_update_stage_other_host"
            other.mkdir()
            (other / "active.txt").write_text("working")
            with patch.object(tempfile, "gettempdir", return_value=directory):
                ns["_cleanup_update_remnants"]()
            self.assertEqual((other / "active.txt").read_text(), "working")
            self.assertFalse((root / "backup").exists())

    def test_existing_dictionary_validation_rejects_bad_values_and_schema(self):
        ns = load_functions({"_load_existing_translations"}, {"json": json, "os": os})
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "existing_zh.json"
            self.assertEqual(ns["_load_existing_translations"](str(path)), {})
            for content in ('{broken', '[]', '{"translations":{}}', json.dumps({
                "$schema": "sp-translation-v1", "language": "zh-CN", "translations": {"Manual": [1]},
            })):
                path.write_text(content, encoding="utf-8")
                with self.assertRaises((ValueError, TypeError)):
                    ns["_load_existing_translations"](str(path))
                self.assertEqual(path.read_text(encoding="utf-8"), content)

    def test_saved_edit_trigger_rejects_bare_keys_and_preserves_modifier_combo(self):
        tree = ast.parse(MODULE.read_text(encoding="utf-8"))
        cls = next(n for n in tree.body if isinstance(n, ast.ClassDef) and n.name == "EditTrigger")
        settings = Mock()
        ns = {"QtCore": SimpleNamespace(QSettings=lambda: settings), "MOUSE_RIGHT": 2}
        exec(compile(ast.Module(body=[cls], type_ignores=[]), str(MODULE), "exec"), ns)
        trigger = ns["EditTrigger"]()
        for saved, expected in (("Z", "Ctrl"), ("Alt+K", "Alt+K"), ("", "")):
            settings.value.side_effect = lambda key, default: saved if key.endswith("edit_key") else 2
            trigger.load()
            self.assertEqual(trigger.key, expected)

    def extractor_methods(self):
        tree = ast.parse(MODULE.read_text(encoding="utf-8"))
        cls = next(n for n in tree.body if isinstance(n, ast.ClassDef)
                   and n.name == "ChineseTranslationToolDialog")
        methods = [n for n in cls.body if isinstance(n, ast.FunctionDef) and n.name in {
            "_read_extractor_output", "_extractor_error", "_extractor_finished"}]
        ns = {"json": json, "os": os, "is_safe": lambda obj: obj is not None, "QT_MAJOR": 6,
              "QtCore": SimpleNamespace(QProcess=SimpleNamespace(ProcessError=SimpleNamespace(FailedToStart=0)))}
        exec(compile(ast.Module(body=methods, type_ignores=[]), str(MODULE), "exec"), ns)
        return ns

    def test_extractor_decodes_utf8_only_after_complete_protocol_line(self):
        ns = self.extractor_methods()
        process = Mock()
        dialog = Mock(_extractor_process=process, _extractor_stdout=b"")
        line = (json.dumps({"type": "warning", "file": "中文路径", "message": "说明"}, ensure_ascii=False) + "\n").encode()
        cut = line.index("中".encode()) + 1
        process.readAllStandardOutput.side_effect = [line[:cut], line[cut:]]
        ns["_read_extractor_output"](dialog)
        dialog.log.appendPlainText.assert_not_called()
        ns["_read_extractor_output"](dialog)
        message = dialog.log.appendPlainText.call_args.args[0]
        self.assertIn("中文路径", message)
        self.assertIn("说明", message)
        self.assertNotIn("\ufffd", message)

    def test_extractor_nonstartup_error_retains_process_until_finished(self):
        ns = self.extractor_methods()
        process = Mock()
        dialog = Mock(_extractor_process=process, _cancelled=False)
        ns["_extractor_error"](dialog, 1)
        self.assertIs(dialog._extractor_process, process)
        dialog._set_running.assert_not_called()
        ns["_extractor_finished"](dialog, 1, None)
        self.assertIsNone(dialog._extractor_process)
        process.deleteLater.assert_called_once()

    def test_extractor_failed_start_releases_process(self):
        ns = self.extractor_methods()
        process = Mock()
        dialog = Mock(_extractor_process=process)
        ns["_extractor_error"](dialog, 0)
        self.assertIsNone(dialog._extractor_process)
        dialog._set_running.assert_called_once_with(False)
        process.deleteLater.assert_called_once()

    def test_build_explicitly_enables_and_targets_extractor(self):
        build = _load_build_module()
        with patch.object(build.shutil, "which", return_value="cmake"), \
                patch.object(build.subprocess, "run") as run, \
                patch.object(Path, "is_file", return_value=False):
            with self.assertRaisesRegex(RuntimeError, "缺少产物"):
                build._build_native()
        configure, compile_call = [call.args[0] for call in run.call_args_list]
        self.assertIn("-DSP_BUILD_EXTRACTOR=ON", configure)
        self.assertIn("-DSP_DEPS_ROOT=" + str(build.DEPS_ROOT), configure)
        self.assertIn("translator_extractor", compile_call)

    @unittest.skipUnless(EXTRACTOR.is_file(), "build the native extractor")
    def test_native_extractor_never_overwrites_invalid_existing_dictionary(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            assets = root / "assets"
            assets.mkdir()
            (assets / "test.glsl").write_text('{"label":"New caption"}')
            output = root / "result_zh.json"
            request = root / "request.json"
            request.write_text(json.dumps({"source": str(assets), "output": str(output), "attributes": ["label"]}))
            for content in ('{broken', json.dumps({"$schema": "sp-translation-v1", "language": "zh-CN",
                                                  "translations": {"Manual": [1]}})):
                output.write_text(content)
                result = subprocess.run([str(EXTRACTOR), "--request", str(request)], capture_output=True, timeout=8)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertEqual(output.read_text(), content)

    @unittest.skipUnless(EXTRACTOR.is_file(), "build the native extractor")
    def test_native_extractor_reads_shader_metadata_inside_archive(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            assets = root / "assets"
            assets.mkdir()
            with zipfile.ZipFile(assets / "bundle.zip", "w") as archive:
                archive.writestr("shaders/test.glsl", '{"label":"Archived caption"}')
            output = root / "result_zh.json"
            request = root / "request.json"
            request.write_text(json.dumps({"source": str(assets), "output": str(output),
                                           "ordinary_filenames": False, "attributes": ["label"]}))
            result = subprocess.run([str(EXTRACTOR), "--request", str(request)], capture_output=True, timeout=8)
            self.assertEqual(result.returncode, 0, result.stdout)
            payload = json.loads(output.read_text(encoding="utf-8"))
            self.assertIn("Archived caption", payload["translations"])
            self.assertEqual(payload["extraction"]["failed_count"], 0)


if __name__ == "__main__":
    unittest.main()
