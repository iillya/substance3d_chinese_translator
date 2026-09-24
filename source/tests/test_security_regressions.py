import ast
import importlib.util
import json
import os
import tempfile
import unittest
import zipfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "source"
MODULE_SOURCE = SOURCE / "substance3d_chinese_translator" / "__init__.py"
CPP_SOURCE = SOURCE / "cpp" / "translation_ui_delegate.cpp"
EXTRACTOR_SOURCE = SOURCE / "cpp" / "extractor.cpp"
ARCHIVE = ROOT / "dist" / "substance3d_chinese_translator.zip"


def _load_build_module():
    path = SOURCE / "cpp" / "build_package.py"
    spec = importlib.util.spec_from_file_location("sp_build_package", path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _load_update_validation_namespace():
    tree = ast.parse(MODULE_SOURCE.read_text(encoding="utf-8"))
    wanted_assignments = {
        "PLUGIN_VERSION", "MAX_UPDATE_FILES", "MAX_UPDATE_FILE_BYTES",
        "MAX_UPDATE_EXPANDED_BYTES", "MAX_UPDATE_COMPRESSION_RATIO",
        "RELEASE_FILE_ALLOWLIST", "REQUIRED_UPDATE_FILES",
        "RELEASE_TRANSLATION_NAMES",
    }
    wanted_functions = {"_normalized_zip_name", "_validate_update_archive"}
    selected = []
    for node in tree.body:
        if (isinstance(node, ast.Assign)
                and any(isinstance(target, ast.Name)
                        and target.id in wanted_assignments
                        for target in node.targets)):
            selected.append(node)
        elif (isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
              and node.name in wanted_functions):
            selected.append(node)
    namespace = {"json": json, "os": os, "zipfile": zipfile}
    exec(compile(ast.Module(body=selected, type_ignores=[]),
                 str(MODULE_SOURCE), "exec"), namespace)
    return namespace


class SecurityRegressionTests(unittest.TestCase):
    def test_release_archive_matches_exact_allowlist(self):
        build = _load_build_module()
        with zipfile.ZipFile(ARCHIVE) as archive:
            names = {name for name in archive.namelist()
                     if not name.endswith("/")}
            self.assertIsNone(archive.testzip())
        self.assertEqual(names, build.RELEASE_FILE_ALLOWLIST)

    def test_runtime_update_validator_accepts_release_and_rejects_extra(self):
        namespace = _load_update_validation_namespace()
        validate = namespace["_validate_update_archive"]
        validate(ARCHIVE, namespace["PLUGIN_VERSION"])
        with tempfile.TemporaryDirectory() as temporary:
            tampered = Path(temporary) / "tampered.zip"
            with zipfile.ZipFile(ARCHIVE) as source, zipfile.ZipFile(
                    tampered, "w", zipfile.ZIP_DEFLATED) as target:
                for info in source.infolist():
                    target.writestr(info.filename, source.read(info.filename))
                target.writestr("foreign_plugin.py", "pass\n")
            with self.assertRaisesRegex(RuntimeError, "非白名单"):
                validate(tampered, namespace["PLUGIN_VERSION"])

    def test_release_version_is_consistent(self):
        metadata = json.loads(
            (SOURCE / "pluginInfo.json").read_text(encoding="utf-8")
        )
        native_metadata = json.loads(
            (SOURCE / "cpp" / "vcpkg.json").read_text(encoding="utf-8")
        )
        tree = ast.parse(MODULE_SOURCE.read_text(encoding="utf-8"))
        version = None
        for node in tree.body:
            if (isinstance(node, ast.Assign)
                    and any(isinstance(target, ast.Name)
                            and target.id == "PLUGIN_VERSION"
                            for target in node.targets)):
                version = ast.literal_eval(node.value)
                break
        self.assertEqual(metadata["version"], version)
        self.assertEqual(native_metadata["version-string"], version)
        self.assertEqual(metadata["min_designer_version"], "14.0.0")

    def test_build_uses_case_deduplicated_environment(self):
        build = _load_build_module()
        source = (SOURCE / "cpp" / "build_package.py").read_text(
            encoding="utf-8"
        )
        self.assertIn("def _build_environment", source)
        self.assertIn("key.casefold()", source)
        self.assertGreaterEqual(source.count("env=_build_environment()"), 2)
        self.assertIsInstance(build._build_environment(), dict)

    def test_update_replaces_release_dictionaries_and_preserves_custom_ones(self):
        namespace = _load_update_validation_namespace()
        self.assertEqual(
            namespace["RELEASE_TRANSLATION_NAMES"],
            {
                "control_ids_zh.json",
                "my_assets_zh.json",
                "official_assets_zh.json",
            },
        )
        source = MODULE_SOURCE.read_text(encoding="utf-8")
        tree = ast.parse(source)
        apply_update = next(
            node for node in tree.body
            if isinstance(node, ast.FunctionDef)
            and node.name == "_apply_update_now"
        )
        apply_source = ast.get_source_segment(source, apply_update)
        self.assertIn(
            "name.casefold() not in RELEASE_TRANSLATION_NAMES",
            apply_source,
        )
        self.assertIn("shutil.copy2(preserved, target)", apply_source)
        self.assertNotIn("_merge_preserved_translation", source)

    def test_update_restores_wait_cursor_before_result_dialogs(self):
        source = MODULE_SOURCE.read_text(encoding="utf-8")
        tree = ast.parse(source)
        apply_update = next(
            node for node in tree.body
            if isinstance(node, ast.FunctionDef)
            and node.name == "_apply_update_now"
        )
        apply_source = ast.get_source_segment(source, apply_update)
        self.assertIn("def restore_wait_cursor():", apply_source)
        self.assertIn("nonlocal wait_cursor_active", apply_source)
        for title in ('"更新已应用"', '"更新失败"'):
            dialog_position = apply_source.index(title)
            restore_position = apply_source.rfind(
                "restore_wait_cursor()", 0, dialog_position
            )
            self.assertGreater(restore_position, 0)

    def test_no_zbrush_connector_code_in_plugin_sources(self):
        forbidden = [
            "z" + "brush_to_painter",
            "substance" + "connector",
            "pix" + "ologic",
        ]
        paths = [
            SOURCE / "__init__.py",
            MODULE_SOURCE,
            SOURCE / "cpp" / "build_package.py",
            CPP_SOURCE,
            EXTRACTOR_SOURCE,
        ]
        combined = "\n".join(
            path.read_text(encoding="utf-8", errors="ignore").casefold()
            for path in paths
        )
        for marker in forbidden:
            self.assertNotIn(marker, combined)

    def test_experimental_asset_model_proxy_is_removed(self):
        combined = (
            MODULE_SOURCE.read_text(encoding="utf-8") +
            CPP_SOURCE.read_text(encoding="utf-8")
        )
        for marker in (
            "ChineseAssetSearch", "ForwardResourceModel",
            "sp_delegate_set_asset_catalog", "_start_asset_catalog_sync",
        ):
            self.assertNotIn(marker, combined)

    def test_painter_cjk_search_keeps_the_native_asset_model(self):
        cpp = CPP_SOURCE.read_text(encoding="utf-8")
        for marker in (
            "AssetRowFilter",
            "AssetSearchManager",
            "Alg::NewResourcesView",
            "Alg::ResourcePickerWidget",
            "Alg::SearchFieldLineEdit",
            "Alg::NewResourceListModel",
            "Pfx::DataBase::ResourceTableWidget",
            "Pfx::DataBase::ResourcesListModel",
            "globalSearch",
            "&QLineEdit::textChanged",
            'QMetaObject::invokeMethod(field_, "textChanged"',
            "setRowHidden",
            "restoreNativeQuery",
            "refreshRowMask",
            "sameSearchSurface",
            "filters_.value(container",
            "filter->setActive(active)",
            "&QAbstractItemModel::dataChanged",
            "visibleQuery.isEmpty() || !containsCjk(visibleQuery)",
            "g_assetRowFilter->shutdown()",
        ):
            self.assertIn(marker, cpp)
        # Asset search must not replace a live view model. The combo popup
        # display wrapper may use an unattached identity proxy solely to pass
        # a translated index to Painter's original delegate.
        executable = "\n".join(
            line for line in cpp.splitlines()
            if not line.lstrip().startswith("//")
        )
        self.assertNotIn("QSortFilterProxyModel", executable)
        self.assertNotIn("view->setModel(", cpp)
        self.assertNotIn("combo->setModel(", cpp)
        self.assertIn("ComboPaintProxyModel", cpp)
        self.assertIn("activeDelegate()->paint(painter, option, displayIndex)", cpp)

    def test_native_global_hook_has_teardown(self):
        cpp = CPP_SOURCE.read_text(encoding="utf-8")
        python = MODULE_SOURCE.read_text(encoding="utf-8")
        self.assertIn("sp_delegate_uninstall_ui", cpp)
        self.assertIn("application->removeEventFilter(g_filter)", cpp)
        self.assertIn("restoreAssetDelegates()", cpp)
        self.assertNotIn("GET_MODULE_HANDLE_EX_FLAG_PIN", cpp)
        self.assertNotIn("QTimer::singleShot(0, qApp", cpp)
        self.assertIn("_uninstall_native_ui()", python)
        self.assertIn("_release_native_delegate()", python)
        self.assertIn("FreeLibrary", python)

    def test_initial_cleanup_does_not_load_native_delegate_twice(self):
        python = MODULE_SOURCE.read_text(encoding="utf-8")
        cleanup = python.split("def _clear_native_shortcuts():", 1)[1].split(
            "\ndef ", 1
        )[0]
        self.assertIn("dll = _native_delegate", cleanup)
        self.assertNotIn("_load_native_delegate()", cleanup)
        self.assertIn(">>> 翻译插件启动完成：", python)
        self.assertNotIn("Translation plugin startup:", python)

    def test_asset_preview_translation_uses_tooltip_context_not_iat_hook(self):
        cpp = CPP_SOURCE.read_text(encoding="utf-8")
        for marker in (
            "struct AssetTooltipContext",
            "QPersistentModelIndex",
            "assetTooltipContextStillMatches",
            "assetTooltipTextWithTranslation",
            "source.toHtmlEscaped()",
            "injectAssetTranslationIntoPreview",
            "isAssetPreviewView",
            "isResourcePickerView(view)",
            "sp_asset_preview_translation",
            "QEvent::ToolTip",
            "QEvent::Show",
            "QEvent::LayoutRequest",
            "QEvent::UpdateRequest",
            "allowHeightGrowth",
            "document->size().height()",
            "sp_asset_preview_original_min_height",
            "label->setMinimumHeight(height)",
            "restoreAssetTooltipDecoration",
            "restoreAllAssetTooltipDecorations",
            "Qt::FindDirectChildrenOnly",
            "sp_asset_preview_document",
            "document->documentLayout()->draw",
        ):
            self.assertIn(marker, cpp)
        for obsolete in (
            "g_pendingTooltipEnglish",
            "g_pendingTooltipDisplay",
            "hookedTooltipShowText",
            "installTooltipShowTextHooks",
            "insertSuffixAfterEnglish",
            'QStringLiteral("\\n\\n中文：")',
            'QStringLiteral("中文：%1")',
            "hasVisibleInjectedAssetTooltip",
            "ASSET SUPPRESS duplicate",
        ):
            self.assertNotIn(obsolete, cpp)

    def test_elided_painter_parameter_labels_use_parent_text_property(self):
        cpp = CPP_SOURCE.read_text(encoding="utf-8")
        for marker in (
            "sourceFromPainterElidedLabel",
            'QStringLiteral("Alg::ElidedLabel")',
            'QStringLiteral("Alg::EditLabel")',
            'parent->property("text")',
            "full.startsWith(prefix, Qt::CaseInsensitive)",
            "label->text().trimmed() == trimmed",
            "if (!full.isEmpty()) source = full;",
        ):
            self.assertIn(marker, cpp)
        # Pure display-layer translation must never write the translated or
        # restored text back into Painter's private label owner.
        self.assertNotIn('owner->setProperty("text", result)', cpp)
        self.assertNotIn('owner->setProperty("text", source)', cpp)

    def test_identity_global_edit_removes_user_override(self):
        cpp = CPP_SOURCE.read_text(encoding="utf-8")
        python = MODULE_SOURCE.read_text(encoding="utf-8")
        self.assertIn("bool removeFallbackTranslation", cpp)
        self.assertIn("g_dictionaryReloadCallback", cpp)
        self.assertIn("refreshTranslatedViews();", cpp)
        self.assertIn("!saveToId && target == source", cpp)
        self.assertIn("translations.remove(source)", cpp)
        self.assertIn("已从 user_added_zh.json 删除该自定义词条", cpp)
        self.assertIn("def _on_native_dictionary_reload", python)
        self.assertIn("load_translation_packages()", python)
        self.assertIn("_sync_native_dictionary()", python)

    def test_translation_source_path_legacy_state_is_removed(self):
        cpp = CPP_SOURCE.read_text(encoding="utf-8")
        python = MODULE_SOURCE.read_text(encoding="utf-8")
        self.assertNotIn("g_translationPaths", cpp)
        self.assertNotIn("sp_delegate_set_translation_path", cpp)
        self.assertNotIn("TRANSLATE_SOURCE_FILES", python)
        self.assertNotIn("sp_delegate_set_translation_path", python)

    def test_designer_asset_export_uses_package_api_not_library_widgets(self):
        source = MODULE_SOURCE.read_text(encoding="utf-8")
        tree = ast.parse(source)
        function = next(
            node for node in tree.body
            if isinstance(node, ast.ClassDef)
            for child in node.body
            if isinstance(child, ast.FunctionDef)
            and child.name == "_export_designer_asset_library_names"
        )
        implementation = ast.get_source_segment(source, function)
        for marker in (
            "sd.getContext()",
            "getSDApplication()",
            "getSDGraphDefinitionMgr()",
            "getGraphDefinitions()",
            "getDefinitions()",
            "definition.getLabel()",
            "getPackageMgr()",
            "getPackages()",
            "getChildrenResources(True)",
            "designer-graph-and-package-api",
            "ResourcesListModel",
            "collect_visible_model",
        ):
            self.assertIn(marker, implementation)
        for forbidden in (
            "_is_library_tree(",
            "_is_resource_list(",
            ".clicked.emit(",
            ".activated.emit(",
            "fetchMore(",
        ):
            self.assertNotIn(forbidden, implementation)

    def test_asset_export_filters_existing_translations_for_both_hosts(self):
        source = MODULE_SOURCE.read_text(encoding="utf-8")
        tree = ast.parse(source)
        helper = next(
            node for node in tree.body
            if isinstance(node, ast.FunctionDef)
            and node.name == "_is_untranslated_asset_name"
        )
        namespace = {
            "TRANSLATE_DICT": {"Known": "已有中文", "Empty": ""},
            "_is_extractable": lambda name: name not in {"skip", ""},
        }
        exec(compile(ast.Module(body=[helper], type_ignores=[]),
                     str(MODULE_SOURCE), "exec"), namespace)
        predicate = namespace["_is_untranslated_asset_name"]
        self.assertFalse(predicate("Known"))
        self.assertTrue(predicate("Empty"))
        self.assertTrue(predicate("New asset"))
        self.assertFalse(predicate("skip"))

        painter = next(
            child for node in tree.body if isinstance(node, ast.ClassDef)
            for child in node.body if isinstance(child, ast.FunctionDef)
            and child.name == "_export_painter_asset_library_names"
        )
        designer = next(
            child for node in tree.body if isinstance(node, ast.ClassDef)
            for child in node.body if isinstance(child, ast.FunctionDef)
            and child.name == "_export_designer_asset_library_names"
        )
        for implementation in (
                ast.get_source_segment(source, painter),
                ast.get_source_segment(source, designer)):
            self.assertIn("_is_untranslated_asset_name(name)", implementation)

    def test_asset_export_does_not_treat_id_only_translation_as_global(self):
        cpp = CPP_SOURCE.read_text(encoding="utf-8")
        start = cpp.index("sp_delegate_is_extractable")
        implementation = cpp[start:cpp.index(
            "sp_delegate_add_translation", start)]
        self.assertIn("g_translations.contains(value)", implementation)
        self.assertIn("ID translations are deliberately scoped", implementation)
        self.assertNotIn("g_idTranslations.cbegin()", implementation)

    def test_extractor_rejects_links_and_special_files(self):
        source = EXTRACTOR_SOURCE.read_text(encoding="utf-8")
        self.assertIn("archive_entry_hardlink(entry)", source)
        self.assertIn("archive_entry_symlink(entry)", source)
        self.assertIn("type != AE_IFREG && type != AE_IFDIR", source)
        self.assertIn("kMaxSourceFiles", source)


if __name__ == "__main__":
    unittest.main()
