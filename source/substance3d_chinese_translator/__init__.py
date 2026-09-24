# -*- coding: utf-8 -*-
"""
Substance 3D Painter / Designer 通用中文翻译补全插件
（资源分类树 + 资产全覆盖版）
支持：Adobe Substance 3D Painter 7.2 至官方最新版、
      Adobe Substance 3D Designer 14 及以上。
Painter 新版使用 PySide6 / Qt6 C++ 显示引擎，旧版自动使用
PySide2 / Qt5 C++ 显示引擎；Designer 固定使用 Qt6 显示引擎。
"""

import ctypes
import gc
import hashlib
import json
import os
import re
import shutil
import tempfile
import threading
import time
import urllib.parse
import urllib.request
import zipfile

# 宿主识别：Designer 提供 ``sd`` 模块，Painter 提供 ``substance_painter``，
# 两个软件中只有一个可导入。合并后的插件在两种宿主中都可安装使用。
try:
    import sd  # noqa: F401  Substance 3D Designer host
    HOST = "designer"
    sp = None
except Exception:
    HOST = "painter"
    try:
        import substance_painter as sp
    except Exception:
        sp = None

try:
    from PySide6 import QtCore, QtGui, QtWidgets
    from shiboken6 import delete, getCppPointer, isValid
    QT_MAJOR = 6
except ImportError:
    from PySide2 import QtCore, QtGui, QtWidgets
    from shiboken2 import delete, getCppPointer, isValid
    QT_MAJOR = 5

WA_DELETE_ON_CLOSE = (QtCore.Qt.WA_DeleteOnClose if QT_MAJOR == 5
                      else QtCore.Qt.WidgetAttribute.WA_DeleteOnClose)
WAIT_CURSOR = (QtCore.Qt.WaitCursor if QT_MAJOR == 5
               else QtCore.Qt.CursorShape.WaitCursor)
WINDOW_MODAL = (QtCore.Qt.WindowModal if QT_MAJOR == 5
                else QtCore.Qt.WindowModality.WindowModal)
QAction = QtWidgets.QAction if QT_MAJOR == 5 else QtGui.QAction
KeyboardModifier = (
    QtCore.Qt.KeyboardModifier if QT_MAJOR >= 6 else QtCore.Qt
)
Key = QtCore.Qt.Key if QT_MAJOR >= 6 else QtCore.Qt
KEY_ESCAPE = Key.Key_Escape
KEY_BACKSPACE = Key.Key_Backspace
KEY_CONTROL = Key.Key_Control
KEY_SHIFT = Key.Key_Shift
KEY_ALT = Key.Key_Alt
KEY_META = Key.Key_Meta
ITEM_DATA_ROLE_DISPLAY = (
    QtCore.Qt.ItemDataRole.DisplayRole if QT_MAJOR >= 6
    else QtCore.Qt.DisplayRole
)


def _enum_int(value):
    """兼容 PySide2/PySide6 的枚举取整。"""
    try:
        return int(value)
    except TypeError:
        return int(value.value)


STRONG_FOCUS = (
    QtCore.Qt.FocusPolicy.StrongFocus if QT_MAJOR >= 6
    else QtCore.Qt.StrongFocus
)
POINTING_HAND = (
    QtCore.Qt.CursorShape.PointingHandCursor if QT_MAJOR >= 6
    else QtCore.Qt.PointingHandCursor
)
EVENT_SHORTCUT_OVERRIDE = (
    QtCore.QEvent.Type.ShortcutOverride if QT_MAJOR >= 6
    else QtCore.QEvent.ShortcutOverride
)
EVENT_KEY_PRESS = (
    QtCore.QEvent.Type.KeyPress if QT_MAJOR >= 6
    else QtCore.QEvent.KeyPress
)
EVENT_KEY_RELEASE = (
    QtCore.QEvent.Type.KeyRelease if QT_MAJOR >= 6
    else QtCore.QEvent.KeyRelease
)
EVENT_MOUSE_BUTTON_PRESS = (
    QtCore.QEvent.Type.MouseButtonPress if QT_MAJOR >= 6
    else QtCore.QEvent.MouseButtonPress
)
EVENT_MOUSE_BUTTON_RELEASE = (
    QtCore.QEvent.Type.MouseButtonRelease if QT_MAJOR >= 6
    else QtCore.QEvent.MouseButtonRelease
)


def _key_sequence_string(key_code, modifiers):
    """把按键码与修饰键组合成 QKeySequence 的可读字符串。"""
    key_int = _enum_int(key_code)
    mod_int = _enum_int(modifiers)
    try:
        return QtGui.QKeySequence(key_int | mod_int).toString()
    except Exception:
        return QtGui.QKeySequence(key_int).toString()


def _modifier_label(modifier):
    """把修饰键整数转成可读文本，如 Ctrl+Alt+Shift。"""
    parts = []
    modifier = int(modifier)
    if modifier & _enum_int(KeyboardModifier.ControlModifier):
        parts.append("Ctrl")
    if modifier & _enum_int(KeyboardModifier.AltModifier):
        parts.append("Alt")
    if modifier & _enum_int(KeyboardModifier.ShiftModifier):
        parts.append("Shift")
    return "+".join(parts)


MouseButton = QtCore.Qt.MouseButton if QT_MAJOR >= 6 else QtCore.Qt
MOUSE_LEFT = _enum_int(MouseButton.LeftButton)
MOUSE_RIGHT = _enum_int(MouseButton.RightButton)
MOUSE_MIDDLE = _enum_int(MouseButton.MiddleButton)
MODIFIER_MASK = (
    _enum_int(KeyboardModifier.ControlModifier)
    | _enum_int(KeyboardModifier.AltModifier)
    | _enum_int(KeyboardModifier.ShiftModifier)
)
EXPANDING_POLICY = (
    QtWidgets.QSizePolicy.Policy.Expanding if QT_MAJOR >= 6
    else QtWidgets.QSizePolicy.Expanding
)


def _modifier_only_sequence(modifiers):
    """把只有修饰键（无主键）的组合转成可读序列，如 Ctrl、Ctrl+Alt。"""
    return _modifier_label(int(modifiers) & MODIFIER_MASK)


def _button_name(button):
    """把鼠标按键整数转成可读文本。"""
    if button == MOUSE_LEFT:
        return "左键"
    if button == MOUSE_RIGHT:
        return "右键"
    if button == MOUSE_MIDDLE:
        return "中键"
    return "按键"


class EditTrigger:
    """“更改翻译弹窗”的触发配置：键盘序列 + 鼠标按键。"""

    SETTINGS_PREFIX = "substance3d_chinese_translator"

    def __init__(self, key=None, button=None):
        self.key = key if key is not None else "Ctrl"
        self.button = (
            int(button) if button is not None else MOUSE_RIGHT
        )

    def display_text(self):
        if self.key and self.button:
            return self.key + "+" + _button_name(self.button)
        return "无"

    def set_trigger(self, key, button):
        self.key = key or ""
        # 0 表示“不绑定鼠标按键”，即禁用该触发方式，不再强制回退为右键。
        self.button = int(button)

    @staticmethod
    def _has_modifier(key):
        """快捷键序列是否包含 Ctrl/Alt/Shift 修饰键。"""
        return any(name in key for name in ("Ctrl", "Alt", "Shift"))

    def restore_default(self):
        self.set_trigger("Ctrl", MOUSE_RIGHT)

    def load(self):
        """从 QSettings 读取；读取失败时保持当前值。"""
        settings = QtCore.QSettings()
        try:
            stored = settings.value(
                self.SETTINGS_PREFIX + "/edit_key", None)
            if stored is None:
                # 旧版兼容：从 edit_modifier 迁移为键盘序列。
                modifier = int(
                    settings.value(
                        self.SETTINGS_PREFIX + "/edit_modifier",
                        _enum_int(KeyboardModifier.ControlModifier),
                    )
                )
                self.key = _modifier_label(modifier)
            else:
                self.key = str(stored)
                # 旧版可能保存过不带修饰键的组合（如 Z+左键），已不再接受，
                # 读取时回退到默认 Ctrl。
                if self.key and not self._has_modifier(self.key):
                    self.key = "Ctrl"
        except (TypeError, ValueError):
            pass
        try:
            button = int(
                settings.value(
                    self.SETTINGS_PREFIX + "/edit_button",
                    self.button,
                )
            )
            self.button = button
        except (TypeError, ValueError):
            pass
        # 旧版“独立键盘快捷键”不再接受，读取时忽略。

    def save(self):
        settings = QtCore.QSettings()
        settings.setValue(
            self.SETTINGS_PREFIX + "/edit_key", self.key
        )
        settings.setValue(
            self.SETTINGS_PREFIX + "/edit_button", self.button
        )

    def apply_to_native(self):
        """把当前配置推送到原生引擎。"""
        return (
            _call_native(
                "sp_delegate_set_edit_key", self.key,
                label="切换更改翻译弹窗触发方式",
            )
            and _call_native(
                "sp_delegate_set_edit_button", self.button,
                label="切换更改翻译弹窗触发方式",
            )
        )


class CaptureButton(QtWidgets.QPushButton):
    """通用快捷键监听按钮：点击进入监听，Esc 取消，Backspace 交给外部恢复默认。

    ``capture_mouse=True`` 时监听“键盘序列+鼠标按键”组合，
    以 (键盘序列, 鼠标按键) 形式通过 ``mouse_captured`` 发出。
    """

    key_captured = QtCore.Signal(str)
    mouse_captured = QtCore.Signal(str, int)
    restore_default = QtCore.Signal()
    cancelled = QtCore.Signal()
    invalid_combination = QtCore.Signal(str)

    def __init__(self, capture_mouse=False, parent=None):
        super().__init__(parent)
        self._capture_mouse = bool(capture_mouse)
        self._normal_text = ""
        self._capturing = False
        self._app = None
        self._pending_key_code = None
        self._pending_seq = None
        self._pending_button = None
        self._pending_text = None
        self.setFocusPolicy(STRONG_FOCUS)
        self.setCursor(POINTING_HAND)
        self.clicked.connect(self._toggle_capture)
        self._refresh_text()

    def set_value(self, text):
        """外部同步当前值文本，不触发保存。"""
        self._normal_text = text if text is not None else ""
        self._refresh_text()

    def _display_text(self):
        if self._capturing:
            if self._pending_text:
                return self._pending_text
            if self._capture_mouse:
                return "请按下快捷键和鼠标组合"
            return "请按下快捷键"
        return self._normal_text or "无"

    def _refresh_text(self):
        self.setText(self._display_text())

    def _toggle_capture(self):
        if self._capturing:
            self._end_capture()
        else:
            self._start_capture()

    def _start_capture(self):
        self._capturing = True
        self._pending_key_code = None
        self._pending_seq = None
        self._pending_button = None
        self._pending_text = None
        self._refresh_text()
        self.setFocus()
        self._app = QtWidgets.QApplication.instance()
        if is_safe(self._app):
            self._app.installEventFilter(self)

    def _end_capture(self):
        if self._capturing and is_safe(self._app):
            self._app.removeEventFilter(self)
        self._capturing = False
        self._pending_key_code = None
        self._pending_seq = None
        self._pending_button = None
        self._pending_text = None
        self._refresh_text()

    def _handle_key(self, event):
        """处理捕获到的按键；返回 True 表示已消费。"""
        key = event.key()
        if key == KEY_ESCAPE:
            self._end_capture()
            self.cancelled.emit()
            return True
        if key == KEY_BACKSPACE:
            self._end_capture()
            self.restore_default.emit()
            return True
        if key in (KEY_CONTROL, KEY_SHIFT, KEY_ALT, KEY_META):
            return True
        if self._capture_mouse:
            # 鼠标监听模式：记录当前按住的按键，等待与鼠标按键组合。
            self._pending_key_code = _enum_int(key)
            return True
        seq = _key_sequence_string(key, event.modifiers())
        self._end_capture()
        if seq:
            self.key_captured.emit(seq)
        else:
            self.cancelled.emit()
        return True

    def _handle_key_release(self, event):
        """释放按键时清除记录的键盘序列，避免未按住的键被误组合。"""
        if not self._capturing or not self._capture_mouse:
            return False
        if (self._pending_key_code is not None
                and _enum_int(event.key()) == self._pending_key_code):
            self._pending_key_code = None
        return True

    def _handle_mouse_press(self, event):
        """键盘序列+鼠标按键组合：记录并在释放时生效，避免原生弹窗同时弹出。"""
        if not self._capture_mouse:
            return False
        button = _enum_int(event.button())
        if button not in (MOUSE_LEFT, MOUSE_RIGHT, MOUSE_MIDDLE):
            return False
        modifiers = _enum_int(event.modifiers())
        if self._pending_key_code is not None:
            seq = _key_sequence_string(self._pending_key_code, modifiers)
        elif modifiers & MODIFIER_MASK:
            seq = _modifier_only_sequence(modifiers)
        else:
            # 只按了鼠标按键（无键盘序列）：不修改当前快捷键。
            return False
        if not seq:
            return False
        if not (modifiers & MODIFIER_MASK):
            # 只接受带 Ctrl/Alt/Shift 修饰键的组合（例如 Ctrl+X+左键）。
            # 无修饰键的字母键（如 Z+左键）会在弹窗打开时把字符灌进输入框，
            # 因此不再允许设置；结束监听、恢复按钮显示当前快捷键，并弹框说明。
            self._end_capture()
            self.invalid_combination.emit(
                "快捷键必须包含 Ctrl/Alt/Shift 修饰键。\n"
                "例如：Ctrl+右键、Ctrl+X+左键、Alt+右键。\n"
                "单独的字母键+鼠标（如 Z+左键）不再支持。"
            )
            return True
        self._pending_seq = seq
        self._pending_button = button
        self._pending_text = seq + "+" + _button_name(button)
        self._refresh_text()
        return True

    def _handle_mouse_release(self, event):
        if not self._capture_mouse:
            return False
        button = _enum_int(event.button())
        if button not in (MOUSE_LEFT, MOUSE_RIGHT, MOUSE_MIDDLE):
            return False
        pending_seq = self._pending_seq
        pending_btn = self._pending_button
        if pending_btn is not None and pending_btn == button:
            self._end_capture()
            self.mouse_captured.emit(pending_seq, pending_btn)
            return True
        return False

    def eventFilter(self, obj, event):
        """监听期间在应用层接管按键，与焦点和宿主快捷键抢占无关。"""
        if not self._capturing:
            return False
        etype = event.type()
        if etype == EVENT_SHORTCUT_OVERRIDE:
            event.accept()
            return True
        if etype == EVENT_KEY_PRESS:
            return self._handle_key(event)
        if etype == EVENT_KEY_RELEASE:
            return self._handle_key_release(event)
        if etype == EVENT_MOUSE_BUTTON_PRESS:
            return self._handle_mouse_press(event)
        if etype == EVENT_MOUSE_BUTTON_RELEASE:
            return self._handle_mouse_release(event)
        return False

    def keyPressEvent(self, event):
        if not self._capturing:
            super().keyPressEvent(event)
            return
        self._handle_key(event)
        event.accept()

    def focusOutEvent(self, event):
        if self._capturing:
            self._end_capture()
        super().focusOutEvent(event)


HOST_DISPLAY_NAME = (
    "Substance 3D Designer" if HOST == "designer"
    else "Substance 3D Painter"
)

IS_APP_QUITTING = False
IS_CLEANING = False
IS_TRANSLATION_ENABLED = True
TRANSLATE_LAYERS_PANEL = True
FUZZY_MATCH_ENABLED = True
FALLBACK_SCAN_ENABLED = False
TRANSLATE_DESIGNER_GRAPH = False
ENABLE_SHORTCUT_DEFAULT = "F10"
ENABLE_SHORTCUT = ENABLE_SHORTCUT_DEFAULT
EDIT_TRIGGER = EditTrigger()
# 统一插件包中本模块位于 <包>/substance3d_chinese_translator/，包根目录是上一层。
MODULE_DIR = os.path.dirname(os.path.abspath(__file__))
PLUGIN_DIR = os.path.dirname(MODULE_DIR)
# 词库统一放在插件包根目录的 translations/ 下，源码与安装包结构一致。
TRANSLATIONS_DIR = os.path.join(PLUGIN_DIR, "translations")
NATIVE_DIR = os.path.join(PLUGIN_DIR, "native")
EXTRACTOR_PATH = os.path.join(NATIVE_DIR, "translator_extractor.exe")
# 统一的 Qt6 翻译引擎同时服务 Painter 10.1+ 与 Designer 14+，
# C++ 侧在运行时自动识别宿主；Qt5 仅用于旧版 Painter。
DELEGATE_DLL_PATH = os.path.join(
    NATIVE_DIR,
    "translator_delegate_qt5.dll" if QT_MAJOR == 5
    else "translator_delegate_qt6.dll",
)
PLUGIN_DISPLAY_NAME = "中文翻译补全插件"
PLUGIN_VERSION = "1.3.7"
PLUGIN_REPO = "iillya/substance3d_chinese_translator"
PLUGIN_RELEASE_URL = (
    f"https://api.github.com/repos/{PLUGIN_REPO}/releases/latest"
)
PLUGIN_ASSET_NAME = "substance3d_chinese_translator.zip"
MAX_UPDATE_DOWNLOAD_BYTES = 64 * 1024 * 1024
MAX_UPDATE_FILES = 256
MAX_UPDATE_FILE_BYTES = 32 * 1024 * 1024
MAX_UPDATE_EXPANDED_BYTES = 96 * 1024 * 1024
MAX_UPDATE_COMPRESSION_RATIO = 250
RELEASE_FILE_ALLOWLIST = {
    "__init__.py",
    "pluginInfo.json",
    "README.md",
    "substance3d_chinese_translator/__init__.py",
    "native/translator_delegate_qt5.dll",
    "native/translator_delegate_qt6.dll",
    "native/translator_extractor.exe",
    "translations/control_ids_zh.json",
    "translations/my_assets_zh.json",
    "translations/official_assets_zh.json",
}
REQUIRED_UPDATE_FILES = RELEASE_FILE_ALLOWLIST
RELEASE_TRANSLATION_NAMES = {
    os.path.basename(path).casefold()
    for path in RELEASE_FILE_ALLOWLIST
    if path.casefold().startswith("translations/")
    and path.casefold().endswith(".json")
}


def _update_scope_id():
    normalized = os.path.normcase(os.path.abspath(PLUGIN_DIR))
    return hashlib.sha256(normalized.encode("utf-8")).hexdigest()[:12]


UPDATE_SCOPE_ID = _update_scope_id()
UPDATE_BACKUP_DIR = os.path.join(
    os.environ.get("LOCALAPPDATA") or tempfile.gettempdir(),
    f"Substance3DChineseTranslationBackup_{UPDATE_SCOPE_ID}",
)
UPDATE_RESULT_FILE = os.path.join(
    tempfile.gettempdir(),
    f"substance3d_update_result_{UPDATE_SCOPE_ID}.txt",
)

# Designer 没有图层面板，图层面板翻译开关仅对 Painter 生效。
if HOST == "designer":
    TRANSLATE_LAYERS_PANEL = False

# Designer 生命周期状态（Painter 复用原有的 _label_extractor_* 全局变量）。
_tool_action = None
_startup_timer = None
_update_notification_timer = None

def _read_bool_setting(key, default):
    """Read a boolean without PySide2's unreliable ``type=bool`` overload."""
    try:
        value = QtCore.QSettings().value(key, default)
    except (Exception, SystemError):
        return bool(default)
    if isinstance(value, bool):
        return value
    if isinstance(value, (int, float)):
        return value != 0
    if isinstance(value, str):
        normalized = value.strip().casefold()
        if normalized in {"1", "true", "yes", "on", "enabled"}:
            return True
        if normalized in {"0", "false", "no", "off", "disabled", ""}:
            return False
    return bool(default)


def _load_saved_settings():
    """从 QSettings 恢复全部用户设置，Painter 与 Designer 共用同一份代码。"""
    global IS_TRANSLATION_ENABLED, TRANSLATE_LAYERS_PANEL
    global FUZZY_MATCH_ENABLED, FALLBACK_SCAN_ENABLED
    global TRANSLATE_DESIGNER_GRAPH
    global ENABLE_SHORTCUT
    IS_TRANSLATION_ENABLED = _read_bool_setting(
        "substance3d_chinese_translator/enabled", True
    )
    if HOST == "painter":
        TRANSLATE_LAYERS_PANEL = _read_bool_setting(
            "substance3d_chinese_translator/translate_layers_panel", True
        )
    FUZZY_MATCH_ENABLED = _read_bool_setting(
        "substance3d_chinese_translator/fuzzy_match", True
    )
    FALLBACK_SCAN_ENABLED = _read_bool_setting(
        "substance3d_chinese_translator/fallback_scan", False
    )
    TRANSLATE_DESIGNER_GRAPH = (
        HOST == "designer" and _read_bool_setting(
            "substance3d_chinese_translator/translate_designer_graph", False
        )
    )
    ENABLE_SHORTCUT = str(
        QtCore.QSettings().value(
            "substance3d_chinese_translator/enable_shortcut",
            ENABLE_SHORTCUT,
        )
    )
    EDIT_TRIGGER.load()


def _get_main_window():
    """获取宿主主窗口（Painter / Designer 共用同一份实现）。"""
    try:
        if HOST == "painter":
            return sp.ui.get_main_window()
        ui_manager = (
            sd.getContext().getSDApplication().getQtForPythonUIMgr()
        )
        if ui_manager is None:
            return None
        return ui_manager.getMainWindow()
    except Exception:
        return None


# ==========================================
# 1. C++ 指针安全校验与图层面板排他判定
# ==========================================
def is_safe(obj):
    """防崩溃核心检查：确保 Python 对象非空且底层 C++ 指针未被销毁"""
    if obj is None:
        return False
    try:
        return isValid(obj)
    except Exception:
        return False


# ==========================================
# 2. JSON translation packages
# ==========================================
TRANSLATE_DICT = {}
ID_TRANSLATE_DICTS = {}


def load_translation_packages():
    """Merge every UTF-8 ``*_zh.json`` package beside this plugin.

    Each package contains a root-level ``translations`` object. Files load
    alphabetically; a later package intentionally overrides duplicate strings.
    """
    TRANSLATE_DICT.clear()
    ID_TRANSLATE_DICTS.clear()
    plugin_dir = TRANSLATIONS_DIR
    if not os.path.isdir(plugin_dir):
        print(f">>> 翻译包目录不存在：{plugin_dir}")
        return
    package_names = sorted(
        (name for name in os.listdir(plugin_dir)
         if name.lower().endswith("_zh.json")),
        key=str.casefold,
    )

    for name in package_names:
        path = os.path.join(plugin_dir, name)
        try:
            with open(path, "r", encoding="utf-8-sig") as stream:
                payload = json.load(stream)
            if not isinstance(payload, dict):
                raise ValueError("package root must be a JSON object")
            if payload.get("$schema") != "sp-translation-v1":
                raise ValueError("unsupported or missing $schema")
            if payload.get("language") != "zh-CN":
                raise ValueError("language must be zh-CN")
            entries = payload.get("translations", {})
            if not isinstance(entries, dict):
                raise ValueError("translations must be a JSON object")
            if not entries:
                raise ValueError("package must contain translations")
            loaded = 0
            if name.lower() == "control_ids_zh.json":
                # ID 专属词库：与全局词库同格式（根级 translations），
                # 键为完整控件 ID（上级类名||自身类名||自身 objectName||原文）。
                for source, target in entries.items():
                    if (isinstance(source, str) and isinstance(target, str)
                            and source and target):
                        ID_TRANSLATE_DICTS[source] = target
                        loaded += 1
            else:
                for source, target in entries.items():
                    if (isinstance(source, str) and isinstance(target, str)
                            and source and target):
                        TRANSLATE_DICT[source] = target
                        loaded += 1
            print(f">>> 已加载翻译包 {name}：{loaded} 条")
        except Exception as exc:
            print(f">>> 跳过无效翻译包 {name}：{exc}")


# Native C++ delegate: Painter never calls back into Python while painting.
# On unload the C++ side removes its global filter/timer, restores every host
# item delegate, and only then releases the DLL reference.
_native_delegate = None
_native_unload_safe = False


def _load_native_delegate():
    global _native_delegate, _native_unload_safe
    if _native_delegate is not None:
        return _native_delegate

    path = DELEGATE_DLL_PATH
    dll = None
    try:
        dll = ctypes.CDLL(path)
        dll.sp_delegate_api_version.argtypes = []
        dll.sp_delegate_api_version.restype = ctypes.c_int
        api_version = dll.sp_delegate_api_version()
        if api_version != 16:
            raise RuntimeError("原生翻译模块 API 不兼容：需要 16，实际 %s" % api_version)
        dll.sp_delegate_replace_translations.argtypes = [ctypes.c_char_p, ctypes.c_int]
        dll.sp_delegate_replace_translations.restype = ctypes.c_int
        dll.sp_delegate_set_fallback_path.argtypes = [ctypes.c_wchar_p]
        dll.sp_delegate_set_fallback_path.restype = None
        dll.sp_delegate_set_id_path.argtypes = [ctypes.c_wchar_p]
        dll.sp_delegate_set_id_path.restype = None
        dll.sp_delegate_set_enabled.argtypes = [ctypes.c_int]
        dll.sp_delegate_set_enabled.restype = None
        dll.sp_delegate_set_fuzzy_match.argtypes = [ctypes.c_int]
        dll.sp_delegate_set_fuzzy_match.restype = None
        dll.sp_delegate_set_fallback_scan.argtypes = [ctypes.c_int]
        dll.sp_delegate_set_fallback_scan.restype = None
        dll.sp_delegate_set_edit_key.argtypes = [ctypes.c_wchar_p]
        dll.sp_delegate_set_edit_key.restype = None
        dll.sp_delegate_is_extractable.argtypes = [ctypes.c_wchar_p]
        dll.sp_delegate_is_extractable.restype = ctypes.c_int
        dll.sp_delegate_set_edit_button.argtypes = [ctypes.c_int]
        dll.sp_delegate_set_edit_button.restype = None
        dll.sp_delegate_set_shortcut_callback.argtypes = [ctypes.c_void_p]
        dll.sp_delegate_set_shortcut_callback.restype = None
        dll.sp_delegate_set_dictionary_reload_callback.argtypes = [ctypes.c_void_p]
        dll.sp_delegate_set_dictionary_reload_callback.restype = None
        dll.sp_delegate_set_enable_shortcut.argtypes = [ctypes.c_wchar_p]
        dll.sp_delegate_set_enable_shortcut.restype = None
        dll.sp_delegate_set_translate_layers.argtypes = [ctypes.c_int]
        dll.sp_delegate_set_translate_layers.restype = None
        dll.sp_delegate_set_translate_designer_graph.argtypes = [ctypes.c_int]
        dll.sp_delegate_set_translate_designer_graph.restype = ctypes.c_int
        dll.sp_delegate_install_ui.argtypes = [ctypes.c_void_p]
        dll.sp_delegate_install_ui.restype = ctypes.c_int
        dll.sp_delegate_uninstall_ui.argtypes = [ctypes.c_void_p]
        dll.sp_delegate_uninstall_ui.restype = ctypes.c_int
        dll.sp_delegate_build_id.restype = ctypes.c_wchar_p
        dll.sp_delegate_build_id.argtypes = []
        print(f">>> 原生翻译引擎构建标识：{dll.sp_delegate_build_id()}")
        _native_delegate = dll
        _native_unload_safe = False
    except Exception as exc:
        print(">>> 原生资源翻译 delegate 加载失败：", exc, sep="")
        _native_delegate = None
        # No engine entry point has run when signature/API validation fails.
        _free_native_handle(dll)
    return _native_delegate


def _call_native(func_name, *args, label="原生调用"):
    """调用原生 DLL 函数，统一处理失败日志。"""
    dll = _load_native_delegate()
    if dll is None:
        return False
    try:
        getattr(dll, func_name)(*args)
        return True
    except Exception as exc:
        print(f">>> {label}失败：", exc, sep="")
        return False


def _sync_native_dictionary():
    dll = _load_native_delegate()
    if dll is None:
        return False
    try:
        _apply_dictionary_reload_callback(dll)
        dll.sp_delegate_set_fallback_path(
            os.path.join(TRANSLATIONS_DIR, "user_added_zh.json")
        )
        dll.sp_delegate_set_id_path(
            os.path.join(TRANSLATIONS_DIR, "control_ids_zh.json")
        )
        dll.sp_delegate_set_translate_layers(int(TRANSLATE_LAYERS_PANEL))
        dll.sp_delegate_set_fuzzy_match(int(FUZZY_MATCH_ENABLED))
        dll.sp_delegate_set_fallback_scan(int(FALLBACK_SCAN_ENABLED))
        EDIT_TRIGGER.apply_to_native()
        payload = json.dumps({
            # An array retains override order when normalized keys collide.
            "translations": [(source, target) for source, target in TRANSLATE_DICT.items()
                             if isinstance(source, str) and isinstance(target, str)],
            "control_ids": {source: target for source, target in ID_TRANSLATE_DICTS.items()
                            if isinstance(source, str) and isinstance(target, str)},
        }, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        if not dll.sp_delegate_replace_translations(payload, len(payload)):
            raise RuntimeError("批量词典同步失败，原生引擎保留上一版词典")
        dll.sp_delegate_set_enabled(int(IS_TRANSLATION_ENABLED))
        return True
    except Exception as exc:
        print(">>> 原生资源翻译字典同步失败：", exc, sep="")
        return False


def _install_native_ui(app):
    dll = _load_native_delegate()
    if dll is None or not is_safe(app):
        return False
    try:
        pointer = getCppPointer(app)[0]
        if not pointer:
            return False
        return dll.sp_delegate_install_ui(ctypes.c_void_p(pointer)) == 1
    except Exception as exc:
        print(">>> C++ 界面翻译引擎安装失败：", exc, sep="")
        return False


def _uninstall_native_ui():
    """移除全局事件过滤器和兜底计时器，避免插件关闭后继续拦截宿主事件。"""
    global _native_unload_safe
    if _native_delegate is None:
        return True
    _native_unload_safe = False
    pointer = 0
    try:
        app = QtWidgets.QApplication.instance()
        if is_safe(app):
            pointer = getCppPointer(app)[0]
        _native_unload_safe = _native_delegate.sp_delegate_uninstall_ui(
            ctypes.c_void_p(pointer or 0)
        ) == 1
    except Exception as exc:
        print(">>> C++ 界面翻译引擎卸载失败：", exc, sep="")
    return _native_unload_safe


def _free_native_handle(dll):
    """Only call for an unused DLL or after confirmed native teardown."""
    handle = getattr(dll, "_handle", 0) if dll is not None else 0
    if handle and os.name == "nt":
        try:
            free_library = ctypes.windll.kernel32.FreeLibrary
            free_library.argtypes = [ctypes.c_void_p]
            free_library.restype = ctypes.c_int
            if free_library(ctypes.c_void_p(handle)):
                dll._handle = 0
                return True
            else:
                print(">>> 原生翻译 DLL 释放失败，将由宿主退出时回收")
        except Exception as exc:
            print(">>> 原生翻译 DLL 释放失败：", exc, sep="")
        return False
    return True


def _release_native_delegate():
    """Do not unload machine code while any native hook/object may retain it."""
    global _native_delegate
    if _native_delegate is None:
        return True
    if not _native_unload_safe:
        print(">>> 原生钩子或对话框尚未完全撤销，保留 DLL，避免卸载后崩溃")
        return False
    if not _free_native_handle(_native_delegate):
        return False
    _native_delegate = None
    gc.collect()
    return True


def _write_json_atomic(path, payload):
    """Write UTF-8 JSON without leaving a partial destination on failure."""
    destination = os.path.abspath(path)
    directory = os.path.dirname(destination) or "."
    os.makedirs(directory, exist_ok=True)
    descriptor, temporary = tempfile.mkstemp(
        prefix=".sp_translation_", suffix=".tmp", dir=directory
    )
    try:
        with os.fdopen(descriptor, "w", encoding="utf-8", newline="\n") as stream:
            json.dump(payload, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, destination)
    except Exception:
        try:
            os.unlink(temporary)
        except OSError:
            pass
        raise


def _ensure_zh_json_suffix(path):
    """确保输出文件以 _zh.json 结尾，避免写出无法被加载的词典文件。"""
    if not str(path).lower().endswith("_zh.json"):
        return os.path.splitext(path)[0] + "_zh.json"
    return path


def _load_existing_translations(path):
    """Read existing entries; refuse to overwrite an unreadable or invalid file."""
    if not os.path.exists(path):
        return {}
    with open(path, "r", encoding="utf-8-sig") as stream:
        payload = json.load(stream)
    if (not isinstance(payload, dict)
            or payload.get("$schema") != "sp-translation-v1"
            or payload.get("language") != "zh-CN"
            or not isinstance(payload.get("translations"), dict)
            or any(not isinstance(value, str) for value in payload["translations"].values())):
        raise ValueError("已有翻译包格式无效，未覆盖：%s" % path)
    return payload["translations"]


def _is_extractable(name):
    """是否值得导出/提取：判定规则统一在 C++，Python 只转发。"""
    dll = _load_native_delegate()
    if dll is None:
        return False
    try:
        return bool(dll.sp_delegate_is_extractable(str(name)))
    except Exception:
        return False


def _is_untranslated_asset_name(name):
    """两端导出共用：只保留尚无有效译文的资产名称。"""
    if not isinstance(name, str):
        return False
    source = name.strip()
    if not source or not _is_extractable(source):
        return False
    translated = TRANSLATE_DICT.get(source)
    return not isinstance(translated, str) or not translated.strip()


def _tooltip(text):
    """悬停提示：整合成一句话，用富文本让 Qt 自动换行。"""
    text = str(text).replace("\r\n", "\n").replace("\n", "")
    escaped = (
        text.replace("&", "&amp;")
        .replace("<", "&lt;")
        .replace(">", "&gt;")
    )
    return "<p>" + escaped + "</p>"


# ---------------------------------------------------------------
# 资源库搜索框的中文→英文反向翻译
# ---------------------------------------------------------------
class ChineseTranslationToolDialog(QtWidgets.QDialog):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowTitle(f"中文翻译工具 v{PLUGIN_VERSION}")
        self.setObjectName("substance3d_chinese_translator_tool")
        self.setMinimumSize(680, 500)
        self.setSizeGripEnabled(True)
        self.setAttribute(WA_DELETE_ON_CLOSE, False)
        self._cancelled = False
        self._extractor_process = None
        self._extractor_request = ""
        self._extractor_stdout = b""
        self._build_ui()

    def _build_ui(self):
        layout = QtWidgets.QVBoxLayout(self)

        credit = QtWidgets.QLabel(
            '<a href="https://space.bilibili.com/281243426" '
            'style="color: #66aaff;">'
            "本插件由 bilibili“神说要凑数”制作，"
            "点击可查看作者主页</a>"
            "&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;"
            '<a href="https://github.com/iillya/'
            'substance3d_chinese_translator" '
            'style="color: #66aaff;">'
            "插件 GitHub 仓库</a>",
            self,
        )
        credit.setOpenExternalLinks(True)
        # 作者署名链接与“检查插件更新”按钮同一行，按钮右对齐。
        top_row = QtWidgets.QHBoxLayout()
        top_row.addWidget(credit, 1)
        self.update_button = QtWidgets.QPushButton("检查插件更新", self)
        self.update_button.setToolTip(
            _tooltip(
                "从 GitHub 检查最新版本。发现新版本时可下载安装包。"
            )
        )
        self.update_button.clicked.connect(
            lambda: _check_updates(self)
        )
        top_row.addWidget(self.update_button)
        layout.addLayout(top_row)

        # 与“快捷键/提取设置/提取选项”保持一致的一级标题样式（无边框）。
        translation_group = QtWidgets.QGroupBox(
            "界面翻译（即时生效）", self
        )
        translation_group.setFlat(True)
        translation_layout = QtWidgets.QVBoxLayout(translation_group)
        self.translation_enabled_check = QtWidgets.QCheckBox(
            f"启用插件翻译（翻译 {HOST_DISPLAY_NAME} 界面）",
            translation_group,
        )
        self.translation_enabled_check.setChecked(IS_TRANSLATION_ENABLED)
        self.translation_enabled_check.setToolTip(
            _tooltip(
                f"勾选时插件翻译生效，自动翻译 {HOST_DISPLAY_NAME} 的界面控件。"
                "取消勾选时停止翻译并立即恢复所有界面原文显示。"
                "仅影响显示，不修改项目数据。"
            )
        )
        self.translation_enabled_check.toggled.connect(
            _set_translation_enabled
        )
        translation_layout.addWidget(self.translation_enabled_check)

        if HOST == "designer":
            self.graph_translation_check = QtWidgets.QCheckBox(
                "启用节点翻译（有未知风险，默认关闭）",
                translation_group,
            )
            self.graph_translation_check.setChecked(
                TRANSLATE_DESIGNER_GRAPH
            )
            self.graph_translation_check.setToolTip(
                _tooltip(
                    "节点标题和端口由 Designer 私有绘制代码生成。"
                    "支持 Designer 14 及以上版本，启用前检查 Qt 绘制入口。"
                    "翻译只作用于绘制出的节点标题和端口。"
                    "关闭后恢复原文，不修改项目数据。"
                )
            )
            self.graph_translation_check.toggled.connect(
                _set_designer_graph_translation
            )
            translation_layout.addWidget(self.graph_translation_check)

        if HOST == "painter":
            self.layers_translation_check = QtWidgets.QCheckBox(
                "翻译图层面板（包括用户创建的图层名称）",
                translation_group,
            )
            self.layers_translation_check.setChecked(TRANSLATE_LAYERS_PANEL)
            self.layers_translation_check.setToolTip(
                _tooltip(
                    "开启后使用图层面板专用规则翻译全部控件和图层名称。"
                    "仅改变显示，不修改项目数据。"
                )
            )
            self.layers_translation_check.toggled.connect(
                _set_layers_panel_translation
            )
            translation_layout.addWidget(self.layers_translation_check)
        self.fuzzy_match_check = QtWidgets.QCheckBox(
            "启用模糊匹配（精准匹配优先，兼容大小写、全半角、下划线等差异）",
            translation_group,
        )
        self.fuzzy_match_check.setChecked(FUZZY_MATCH_ENABLED)
        self.fuzzy_match_check.setToolTip(
            _tooltip(
                "勾选后，当词库中找不到完全相同的原文时，"
                "会忽略大小写、全角/半角、下划线、空格差异（多余或缺少）、"
                "省略号等差异进行匹配。"
                "词库中的精准词条始终优先。"
            )
        )
        self.fuzzy_match_check.toggled.connect(_set_fuzzy_match)
        translation_layout.addWidget(self.fuzzy_match_check)
        self.fallback_scan_check = QtWidgets.QCheckBox(
            "启用全量扫描兜底（每 10 秒一次，翻译有漏网时启用）",
            translation_group,
        )
        self.fallback_scan_check.setChecked(FALLBACK_SCAN_ENABLED)
        self.fallback_scan_check.setToolTip(
            _tooltip(
                "勾选后插件每 10 秒扫描一次全部可见控件补翻译。"
                "正常情况下界面事件已能覆盖所有翻译，一般无需开启。"
            )
        )
        self.fallback_scan_check.toggled.connect(_set_fallback_scan)
        translation_layout.addWidget(self.fallback_scan_check)
        hint_text = (
            "提示：取消勾选“启用插件翻译”后，整个界面立即恢复英文原文。"
            "仅关闭“翻译图层面板”则只恢复图层面板中的原文。"
            if HOST == "painter"
            else "提示：取消勾选“启用插件翻译”后，整个界面立即恢复英文原文。"
        )
        translation_hint = QtWidgets.QLabel(hint_text, translation_group)
        translation_hint.setWordWrap(True)
        translation_hint.setStyleSheet("color: gray;")
        translation_layout.addWidget(translation_hint)
        layout.addWidget(translation_group)
        self._update_translation_controls()

        shortcut_group = QtWidgets.QGroupBox("快捷键", self)
        shortcut_form = QtWidgets.QFormLayout(shortcut_group)
        self.enable_shortcut_button = CaptureButton(
            capture_mouse=False, parent=shortcut_group
        )
        self.enable_shortcut_button.setMinimumWidth(160)
        self.enable_shortcut_button.set_value(ENABLE_SHORTCUT)
        self.enable_shortcut_button.setToolTip(
            _tooltip("按 Esc 取消，按 Backspace 恢复默认。")
        )
        self.enable_shortcut_button.key_captured.connect(
            self._apply_enable_shortcut
        )
        self.enable_shortcut_button.restore_default.connect(
            lambda: self._apply_enable_shortcut(ENABLE_SHORTCUT_DEFAULT)
        )

        self.edit_popup_button = CaptureButton(
            capture_mouse=True, parent=shortcut_group
        )
        self.edit_popup_button.setMinimumWidth(160)
        self.edit_popup_button.set_value(EDIT_TRIGGER.display_text())
        self.edit_popup_button.setToolTip(
            _tooltip("按 Esc 取消，按 Backspace 恢复默认。")
        )
        self.edit_popup_button.mouse_captured.connect(
            self._apply_edit_trigger
        )
        self.edit_popup_button.invalid_combination.connect(
            self._show_invalid_shortcut_hint
        )
        self.edit_popup_button.restore_default.connect(
            self._restore_edit_trigger_default
        )

        # 两个快捷键选项横向等分：各自占据一行的一半，按钮在各自半区内展开。
        shortcut_row = QtWidgets.QWidget(shortcut_group)
        row_layout = QtWidgets.QHBoxLayout(shortcut_row)
        row_layout.setContentsMargins(0, 0, 0, 0)
        row_layout.setSpacing(24)

        def make_shortcut_cell(label_text, button):
            cell = QtWidgets.QWidget(shortcut_row)
            cell_layout = QtWidgets.QHBoxLayout(cell)
            cell_layout.setContentsMargins(0, 0, 0, 0)
            cell_layout.setSpacing(8)
            label = QtWidgets.QLabel(label_text, cell)
            # 两个标签取相同最小宽度，保证两个按钮在各自半区内等宽。
            label.setMinimumWidth(130)
            cell_layout.addWidget(label)
            cell_layout.addWidget(button, 1)
            policy = button.sizePolicy()
            policy.setHorizontalPolicy(EXPANDING_POLICY)
            button.setSizePolicy(policy)
            return cell

        row_layout.addWidget(
            make_shortcut_cell("启用/禁用插件翻译",
                               self.enable_shortcut_button),
            1,
        )
        row_layout.addWidget(
            make_shortcut_cell("更改翻译弹窗", self.edit_popup_button),
            1,
        )
        shortcut_form.addRow(shortcut_row)
        layout.addWidget(shortcut_group)

        extract_group = QtWidgets.QGroupBox("提取设置", self)
        form = QtWidgets.QFormLayout(extract_group)
        self.folder_edit = QtWidgets.QLineEdit()
        folder_row = QtWidgets.QHBoxLayout()
        folder_row.addWidget(self.folder_edit, 1)
        self.folder_button = QtWidgets.QPushButton("浏览…", self)
        self.folder_button.clicked.connect(self._browse_folder)
        folder_row.addWidget(self.folder_button)
        form.addRow("资产目录", folder_row)

        self.output_edit = QtWidgets.QLineEdit()
        output_row = QtWidgets.QHBoxLayout()
        output_row.addWidget(self.output_edit, 1)
        self.output_button = QtWidgets.QPushButton("选择…", self)
        self.output_button.clicked.connect(self._browse_output)
        output_row.addWidget(self.output_button)
        form.addRow("输出 JSON", output_row)

        self.package_id_edit = QtWidgets.QLineEdit("extracted-assets")
        form.addRow("翻译包 ID", self.package_id_edit)
        self.description_edit = QtWidgets.QLineEdit("提取的 Substance 资产标签")
        form.addRow("说明", self.description_edit)
        layout.addWidget(extract_group)

        options_group = QtWidgets.QGroupBox("提取选项", self)
        options_form = QtWidgets.QFormLayout(options_group)

        self.filename_check = QtWidgets.QCheckBox("提取文件名")
        self.filename_check.setChecked(True)
        self.foldername_check = QtWidgets.QCheckBox("提取文件夹名")
        self.foldername_check.setChecked(False)
        self.label_check = QtWidgets.QCheckBox("提取 label")
        self.label_check.setChecked(True)
        self.text_check = QtWidgets.QCheckBox("提取 text")
        self.text_check.setChecked(True)
        self.group_check = QtWidgets.QCheckBox("提取 group")
        self.group_check.setChecked(True)
        self.description_check = QtWidgets.QCheckBox("提取 description")
        self.description_check.setChecked(False)
        self.category_check = QtWidgets.QCheckBox("提取 category")
        self.category_check.setChecked(True)
        self.keywords_check = QtWidgets.QCheckBox("提取 keywords")
        self.keywords_check.setChecked(False)
        self.values_check = QtWidgets.QCheckBox("提取 values")
        self.values_check.setChecked(True)
        self.disabled_description_check = QtWidgets.QCheckBox(
            "提取 description_disabled"
        )
        self.disabled_description_check.setChecked(False)

        # 所有内容选项统一排列，每行 3 个。
        content_widget = QtWidgets.QWidget(options_group)
        content_grid = QtWidgets.QGridLayout(content_widget)
        content_grid.setContentsMargins(0, 0, 0, 0)
        content_grid.setHorizontalSpacing(24)
        content_grid.setVerticalSpacing(6)
        content_checks = [
            self.filename_check,
            self.foldername_check,
            self.label_check,
            self.text_check,
            self.group_check,
            self.values_check,
            self.category_check,
            self.keywords_check,
            self.description_check,
            self.disabled_description_check,
        ]
        for index, checkbox in enumerate(content_checks):
            content_grid.addWidget(checkbox, index // 3, index % 3)
        options_form.addRow(content_widget)
        layout.addWidget(options_group)

        note = QtWidgets.QLabel(
            "递归扫描 Substance 资源文件，并解析 SBS、SBSAR、SPSM、SPPR、"
            "GLSL 等受支持格式。\n只提取不含中文且不在当前插件词库中的原文。\n"
            "若输出字典已经存在，将保留其中已有译文并追加新词条。",
            self,
        )
        note.setWordWrap(True)
        layout.addWidget(note)

        self.progress = QtWidgets.QProgressBar()
        self.progress.setRange(0, 1)
        self.progress.setValue(0)
        layout.addWidget(self.progress)
        self.status_label = QtWidgets.QLabel("就绪")
        layout.addWidget(self.status_label)
        self.log = QtWidgets.QPlainTextEdit()
        self.log.setReadOnly(True)
        self.log.setMaximumBlockCount(5000)
        layout.addWidget(self.log, 1)

        self.button_container = QtWidgets.QWidget(self)
        button_layout = QtWidgets.QHBoxLayout(self.button_container)
        button_layout.setContentsMargins(0, 0, 0, 0)
        self.open_translations_button = QtWidgets.QPushButton(
            "打开插件词条目录", self.button_container
        )
        self.open_translations_button.clicked.connect(
            self._open_translations_directory
        )
        button_layout.addWidget(self.open_translations_button)
        self.export_library_button = QtWidgets.QPushButton(
            "导出资产库未翻译名称", self.button_container
        )
        self.export_library_button.setToolTip(
            _tooltip("导出资源库中尚无有效中文译文的资产名称。")
        )
        self.export_library_button.clicked.connect(
            self._export_asset_library_names
        )
        button_layout.addWidget(self.export_library_button)
        button_layout.addStretch(1)
        self.extract_button = QtWidgets.QPushButton("开始提取", self.button_container)
        self.cancel_button = QtWidgets.QPushButton("取消", self.button_container)
        self.close_button = QtWidgets.QPushButton("关闭", self.button_container)
        button_layout.addWidget(self.extract_button)
        button_layout.addWidget(self.cancel_button)
        button_layout.addWidget(self.close_button)
        self.cancel_button.setEnabled(False)
        self.extract_button.clicked.connect(self._start)
        self.cancel_button.clicked.connect(self._cancel)
        self.close_button.clicked.connect(self.close)
        layout.addWidget(self.button_container)

    def _update_translation_controls(self):
        """按总开关状态联动图层翻译子控件是否可用。"""
        master_on = self.translation_enabled_check.isChecked()
        if (hasattr(self, "layers_translation_check")
                and is_safe(self.layers_translation_check)):
            self.layers_translation_check.setEnabled(master_on)
        if (hasattr(self, "graph_translation_check")
                and is_safe(self.graph_translation_check)):
            self.graph_translation_check.setEnabled(master_on)

    def _apply_enable_shortcut(self, value):
        global ENABLE_SHORTCUT
        ENABLE_SHORTCUT = str(value or "")
        QtCore.QSettings().setValue(
            "substance3d_chinese_translator/enable_shortcut",
            ENABLE_SHORTCUT,
        )
        _apply_shortcuts()
        self.enable_shortcut_button.set_value(ENABLE_SHORTCUT)

    def _apply_edit_trigger(self, key_seq, button):
        EDIT_TRIGGER.set_trigger(key_seq, button)
        EDIT_TRIGGER.save()
        EDIT_TRIGGER.apply_to_native()
        _apply_shortcuts()
        self.edit_popup_button.set_value(EDIT_TRIGGER.display_text())

    def _restore_edit_trigger_default(self):
        EDIT_TRIGGER.restore_default()
        EDIT_TRIGGER.save()
        EDIT_TRIGGER.apply_to_native()
        _apply_shortcuts()
        self.edit_popup_button.set_value(EDIT_TRIGGER.display_text())

    def _show_invalid_shortcut_hint(self, message):
        """快捷键不符合规则时弹出提示框（延后一拍，避免在事件过滤中弹窗）。"""
        QtCore.QTimer.singleShot(
            0,
            lambda: (
                QtWidgets.QMessageBox.warning(self, "快捷键无效", message)
                if is_safe(self) else None
            ),
        )

    def _open_translations_directory(self):
        os.makedirs(TRANSLATIONS_DIR, exist_ok=True)
        QtGui.QDesktopServices.openUrl(
            QtCore.QUrl.fromLocalFile(os.path.normpath(TRANSLATIONS_DIR))
        )

    def _export_asset_library_names(self):
        """导出资源库中尚无中文译文的资产名称，按宿主分发。"""
        if HOST == "designer":
            self._export_designer_asset_library_names()
        else:
            self._export_painter_asset_library_names()

    def _export_painter_asset_library_names(self):
        initial = os.path.join(TRANSLATIONS_DIR, "untranslated_assets_zh.json")
        output, _ = QtWidgets.QFileDialog.getSaveFileName(
            self, "导出 Painter 资产库未翻译名称", initial,
            "中文翻译包 (*_zh.json)"
        )
        if not output:
            return
        output = _ensure_zh_json_suffix(output)

        names = set()
        failures = []
        seen = set()

        def resource_name(resource, identifier):
            """Read the display name across Painter's old and new APIs."""
            gui_name = getattr(resource, "gui_name", None)
            if callable(gui_name):
                value = gui_name()
                if isinstance(value, str) and value.strip():
                    return value.strip()
            # Painter 7.x Resource only exposes ResourceID.name. In that API
            # Shelf.resources() is already flat, so this is the asset caption
            # used by the Assets window.
            value = getattr(identifier, "name", "")
            if callable(value):
                value = value()
            return value.strip() if isinstance(value, str) else ""

        def visit(resource, depth=0):
            if depth > 64:
                return
            identifier = None
            try:
                identifier = resource.identifier()
                identity = identifier.url() if hasattr(identifier, "url") else repr(identifier)
            except Exception:
                identity = repr(resource)
            if identity in seen:
                return
            seen.add(identity)
            try:
                name = resource_name(resource, identifier)
                if _is_untranslated_asset_name(name):
                    names.add(name)
            except Exception as exc:
                failures.append(str(exc))
            children = getattr(resource, "children", None)
            if callable(children):
                try:
                    for child in children():
                        visit(child, depth + 1)
                except Exception as exc:
                    failures.append(str(exc))

        QtWidgets.QApplication.setOverrideCursor(WAIT_CURSOR)
        try:
            shelves = list(sp.resource.Shelves.all())
            for shelf_index, shelf in enumerate(shelves, 1):
                try:
                    for resource in shelf.resources():
                        visit(resource)
                except Exception as exc:
                    failures.append(f"{shelf.name()}: {exc}")
                self.status_label.setText(
                    f"正在读取资产库 {shelf_index}/{len(shelves)}，已发现 {len(names)} 个名称"
                )
                QtWidgets.QApplication.processEvents()

            existing = _load_existing_translations(output)
            translations = dict(existing)
            for name in names:
                translations.setdefault(name, "")
            payload = {
                "$schema": "sp-translation-v1",
                "id": "painter-untranslated-assets",
                "language": "zh-CN",
                "description": "Untranslated asset names exported through the Substance 3D Painter resource API",
                "extraction": {
                    "shelf_count": len(shelves),
                    "resource_count": len(seen),
                    "term_count": len(names),
                    "failed_count": len(failures),
                },
                "translations": dict(sorted(translations.items(), key=lambda item: item[0].casefold())),
            }
            _write_json_atomic(output, payload)
            self.output_edit.setText(output)
            self.status_label.setText(f"资产库导出完成：{len(names)} 条词条")
            self.log.appendPlainText(
                f"资产库导出  {output}  [资产 {len(seen)}，词条 {len(names)}，失败 {len(failures)}]"
            )
        except Exception as exc:
            QtWidgets.QMessageBox.critical(self, "资产库导出失败", str(exc))
        finally:
            QtWidgets.QApplication.restoreOverrideCursor()

    def _export_designer_asset_library_names(self):
        """通过 Designer API 导出已注册节点定义及已加载包资源。"""
        initial = os.path.join(TRANSLATIONS_DIR, "untranslated_assets_zh.json")
        output, _ = QtWidgets.QFileDialog.getSaveFileName(
            self, "导出 Designer 资源库未翻译名称", initial,
            "中文翻译包 (*_zh.json)"
        )
        if not output:
            return
        output = _ensure_zh_json_suffix(output)

        QtWidgets.QApplication.setOverrideCursor(WAIT_CURSOR)
        try:
            names = set()
            failures = []
            seen = set()
            context = sd.getContext()
            application = context.getSDApplication()
            graph_manager = application.getSDGraphDefinitionMgr()
            package_manager = application.getPackageMgr()
            modules = list(graph_manager.getGraphDefinitions())
            packages = list(package_manager.getPackages())
            visible_model_count = 0
            visible_item_count = 0

            # 资源库面板中的滤镜、生成器等是节点定义，而不是 SDPackage
            # 资源；从定义管理器读取 label 才能得到用户看到的名称。
            definition_count = 0
            for module_number, module in enumerate(modules, 1):
                try:
                    definitions = module.getDefinitions()
                except Exception as exc:
                    failures.append("读取节点模块失败：%s" % exc)
                    continue
                for definition in definitions:
                    try:
                        name = str(definition.getLabel()).strip()
                        identity = "definition:%s:%s" % (
                            id(module), name
                        )
                        if identity in seen:
                            continue
                        seen.add(identity)
                        definition_count += 1
                        if _is_untranslated_asset_name(name):
                            names.add(name)
                    except Exception as exc:
                        failures.append("读取节点名称失败：%s" % exc)
                self.status_label.setText(
                    f"正在读取节点模块 {module_number}/{len(modules)}，"
                    f"已发现 {len(names)} 个名称"
                )
                QtWidgets.QApplication.processEvents()

            # SD 16 can report no public definitions/packages even while its
            # shelf is populated.  In that case the visible Pfx model is the
            # only runtime catalog exposed by the host.  Read it directly;
            # do not synthesize clicks, change the selected category, or scan
            # files on disk.
            model_ids = set()

            def collect_visible_model(model):
                nonlocal visible_item_count

                def visit(index, depth=0):
                    nonlocal visible_item_count
                    if depth > 64:
                        return
                    visible_item_count += 1
                    try:
                        name = index.data(ITEM_DATA_ROLE_DISPLAY)
                    except Exception:
                        name = None
                    if isinstance(name, str) and _is_untranslated_asset_name(name):
                        names.add(name.strip())
                    try:
                        rows = model.rowCount(index)
                    except Exception:
                        return
                    for row in range(rows):
                        child = model.index(row, 0, index)
                        if child.isValid():
                            visit(child, depth + 1)

                for row in range(model.rowCount()):
                    index = model.index(row, 0)
                    if index.isValid():
                        visit(index)

            for widget in QtWidgets.QApplication.allWidgets():
                if not isinstance(widget, QtWidgets.QAbstractItemView):
                    continue
                try:
                    model = widget.model()
                except Exception:
                    continue
                if (model is None or id(model) in model_ids
                        or not _class_name(model).endswith(
                            "ResourcesListModel")):
                    continue
                model_ids.add(id(model))
                visible_model_count += 1
                try:
                    collect_visible_model(model)
                except Exception as exc:
                    failures.append("读取资源库模型失败：%s" % exc)

            for package_number, package in enumerate(packages, 1):
                try:
                    resources = package.getChildrenResources(True)
                except Exception as exc:
                    failures.append("读取包资源失败：%s" % exc)
                    continue
                for resource in resources:
                    try:
                        name = str(resource.getIdentifier()).strip()
                        identity = "%s:%s" % (id(package), name)
                        if identity in seen:
                            continue
                        seen.add(identity)
                        if _is_untranslated_asset_name(name):
                            names.add(name)
                    except Exception as exc:
                        failures.append("读取资源名称失败：%s" % exc)
                self.status_label.setText(
                    f"正在读取已加载资源包 {package_number}/{len(packages)}，"
                    f"已发现 {len(names)} 个名称"
                )
                QtWidgets.QApplication.processEvents()

            existing = _load_existing_translations(output)
            translations = dict(existing)
            for name in names:
                translations.setdefault(name, "")
            payload = {
                "$schema": "sp-translation-v1",
                "id": "designer-untranslated-assets",
                "language": "zh-CN",
                "description": (
                    "Untranslated asset names exported from the "
                    "Substance 3D Designer library"
                ),
                "extraction": {
                    "source": "designer-graph-and-package-api",
                    "module_count": len(modules),
                    "definition_count": definition_count,
                    "package_count": len(packages),
                    "resource_count": len(seen),
                    "visible_model_count": visible_model_count,
                    "visible_item_count": visible_item_count,
                    "term_count": len(names),
                    "failed_count": len(failures),
                },
                "translations": dict(sorted(
                    translations.items(), key=lambda item: item[0].casefold()
                )),
            }
            _write_json_atomic(output, payload)
            self.output_edit.setText(output)
            self.status_label.setText(
                f"资产库导出完成：{len(names)} 条词条"
            )
            self.log.appendPlainText(
                f"资产库导出  {output}  "
                f"[节点模块 {len(modules)}，节点 {definition_count}，"
                f"已加载包 {len(packages)}，资源 {len(seen)}，"
                f"资源列表 {visible_item_count}，"
                f"词条 {len(names)}，失败 {len(failures)}]"
            )
        except Exception as exc:
            QtWidgets.QMessageBox.critical(
                self, "资产库导出失败", str(exc)
            )
        finally:
            QtWidgets.QApplication.restoreOverrideCursor()

    def _browse_folder(self):
        folder = QtWidgets.QFileDialog.getExistingDirectory(self, "选择 Substance 资产目录", self.folder_edit.text())
        if folder:
            self.folder_edit.setText(folder)
            if not self.output_edit.text().strip():
                self.output_edit.setText(os.path.join(folder, "extracted_assets_zh.json"))

    def _browse_output(self):
        initial = self.output_edit.text().strip() or "extracted_assets_zh.json"
        path, _ = QtWidgets.QFileDialog.getSaveFileName(
            self, "保存翻译包", initial, "中文翻译包 (*_zh.json)"
        )
        if path:
            path = _ensure_zh_json_suffix(path)
            self.output_edit.setText(path)

    def _set_running(self, running):
        self.extract_button.setEnabled(not running)
        self.cancel_button.setEnabled(running)
        self.folder_edit.setEnabled(not running)
        self.output_edit.setEnabled(not running)
        self.package_id_edit.setEnabled(not running)
        self.description_edit.setEnabled(not running)
        self.filename_check.setEnabled(not running)
        self.foldername_check.setEnabled(not running)
        self.label_check.setEnabled(not running)
        self.text_check.setEnabled(not running)
        self.group_check.setEnabled(not running)
        self.description_check.setEnabled(not running)
        self.category_check.setEnabled(not running)
        self.keywords_check.setEnabled(not running)
        self.values_check.setEnabled(not running)
        self.disabled_description_check.setEnabled(not running)
        self.folder_button.setEnabled(not running)
        self.output_button.setEnabled(not running)
        self.export_library_button.setEnabled(not running)

    def _start(self):
        folder_text = self.folder_edit.text().strip()
        if not folder_text:
            QtWidgets.QMessageBox.warning(
                self, "无法开始", "请先选择资产目录。"
            )
            return
        folder = os.path.abspath(folder_text)
        output = os.path.abspath(self.output_edit.text().strip())
        attributes = []
        if self.label_check.isChecked():
            attributes.append("label")
        if self.text_check.isChecked():
            attributes.append("text")
        if self.group_check.isChecked():
            attributes.append("group")
        if self.description_check.isChecked():
            attributes.append("description")
        if self.category_check.isChecked():
            attributes.append("category")
        if self.keywords_check.isChecked():
            attributes.append("keywords")
        if self.values_check.isChecked():
            attributes.append("values")
        if self.disabled_description_check.isChecked():
            attributes.append("description_disabled")
        if not os.path.isdir(folder):
            QtWidgets.QMessageBox.warning(self, "无法开始", "请选择有效的资产目录。")
            return
        # 防止误选磁盘根目录导致扫描整个分区。
        if os.path.dirname(folder) == folder:
            QtWidgets.QMessageBox.warning(
                self, "无法开始",
                "不能直接选择磁盘根目录作为资产目录，请选择具体的文件夹。",
            )
            return
        if not output.lower().endswith("_zh.json"):
            QtWidgets.QMessageBox.warning(self, "无法开始", "输出文件名必须以 _zh.json 结尾。")
            return
        if not self.package_id_edit.text().strip():
            QtWidgets.QMessageBox.warning(self, "无法开始", "翻译包 ID 不能为空。")
            return
        if not os.path.isfile(EXTRACTOR_PATH):
            QtWidgets.QMessageBox.critical(
                self, "无法开始", "缺少 C++ 词条提取器。请重新安装插件。"
            )
            return
        # The native worker validates/enumerates the directory once. A Python
        # os.walk here would repeat that I/O and block the host UI on slow disks.
        excluded = set(TRANSLATE_DICT)
        # Scoped UI translations do not translate asset names globally.
        # Match the native export rule: only global entries are excluded.
        descriptor, request_path = tempfile.mkstemp(
            prefix="sp_translation_request_", suffix=".json"
        )
        os.close(descriptor)
        request = {
            "source": folder,
            "output": output,
            "package_id": self.package_id_edit.text().strip(),
            "description": self.description_edit.text().strip(),
            "ordinary_filenames": self.filename_check.isChecked(),
            "folder_names": self.foldername_check.isChecked(),
            "attributes": attributes,
            "excluded": sorted(excluded, key=str.casefold),
        }
        try:
            _write_json_atomic(request_path, request)
        except Exception as exc:
            try:
                os.remove(request_path)
            except OSError:
                pass
            QtWidgets.QMessageBox.critical(self, "无法开始", str(exc))
            return

        self._cancelled = False
        self.log.clear()
        self.progress.setRange(0, 0)
        self.progress.setValue(0)
        self._set_running(True)
        self.status_label.setText("正在启动 C++ 词条提取器…")
        self._extractor_request = request_path
        self._extractor_stdout = b""
        process = QtCore.QProcess(self)
        self._extractor_process = process
        process.readyReadStandardOutput.connect(
            self._read_extractor_output
        )
        process.readyReadStandardError.connect(
            self._read_extractor_error
        )
        process.finished.connect(self._extractor_finished)
        process.errorOccurred.connect(self._extractor_error)
        process.start(EXTRACTOR_PATH, ["--request", request_path])

    def _read_extractor_output(self):
        process = self._extractor_process
        if not is_safe(process):
            return
        self._extractor_stdout += bytes(
            process.readAllStandardOutput()
        )
        # QProcess may split a UTF-8 character across reads. Decode complete
        # protocol lines, not arbitrary byte chunks.
        while b"\n" in self._extractor_stdout:
            line, self._extractor_stdout = self._extractor_stdout.split(
                b"\n", 1
            )
            line = line.decode("utf-8", errors="replace").strip()
            if not line:
                continue
            try:
                message = json.loads(line)
            except Exception:
                self.log.appendPlainText(line)
                continue
            message_type = message.get("type")
            if message_type == "progress":
                try:
                    current = int(message.get("current", 0))
                except (TypeError, ValueError):
                    current = 0
                try:
                    total = max(0, int(message.get("total", 0)))
                except (TypeError, ValueError):
                    total = 0
                self.progress.setRange(0, max(1, total))
                self.progress.setValue(current)
                try:
                    name = os.path.relpath(
                        message.get("file", ""),
                        self.folder_edit.text().strip(),
                    )
                except Exception:
                    name = message.get("file", "")
                self.status_label.setText(
                    f"[{current}/{total}] {name}"
                )
            elif message_type == "warning":
                self.log.appendPlainText(
                    f"失败  {message.get('file', '')}  "
                    f"[{message.get('message', '')}]"
                )
            elif message_type == "success":
                added = int(message.get("terms", 0))
                self.log.appendPlainText(
                    f"成功  {message.get('file', '')}  [新增 {added} 条]"
                )
            elif message_type == "finished":
                self.status_label.setText(
                    f"完成：新增 {message.get('terms', 0)} 条，"
                    f"失败 {message.get('failures', 0)} 个"
                )
                self.log.appendPlainText(
                    f"\n已写入：{message.get('output', '')}"
                )
            elif message_type == "fatal":
                self.log.appendPlainText(
                    f"致命错误：{message.get('message', '')}"
                )

    def _read_extractor_error(self):
        process = self._extractor_process
        if is_safe(process):
            text = bytes(process.readAllStandardError()).decode(
                "utf-8", errors="replace"
            ).strip()
            if text:
                self.log.appendPlainText(text)

    def _cleanup_extractor_request(self):
        if self._extractor_request:
            try:
                os.remove(self._extractor_request)
            except OSError:
                pass
            self._extractor_request = ""

    def _extractor_finished(self, exit_code, _exit_status):
        process = self._extractor_process
        self._read_extractor_output()
        self._read_extractor_error()
        self._cleanup_extractor_request()
        self._set_running(False)
        if self._cancelled:
            self.status_label.setText("提取已停止；若取消前已完成写入，输出文件会保留。")
        elif exit_code != 0:
            self.status_label.setText(f"提取失败（错误码 {exit_code}）")
        self._extractor_process = None
        if is_safe(process):
            process.deleteLater()

    def _extractor_error(self, error):
        process = self._extractor_process
        if not is_safe(process):
            return
        error_text = process.errorString()
        self.log.appendPlainText(f"提取器错误：{error_text}")
        failed_to_start = (QtCore.QProcess.ProcessError.FailedToStart if QT_MAJOR >= 6
                           else QtCore.QProcess.FailedToStart)
        if error != failed_to_start:
            # Crashed/read/write errors still have a live process or a pending
            # finished signal. Keep ownership until that final cleanup.
            return
        # QProcess 启动失败时（如提取器被占用）不会触发 finished()，
        # 这里补做收尾，避免界面一直停在“运行中”。
        self._read_extractor_output()
        self._cleanup_extractor_request()
        self._set_running(False)
        self.status_label.setText(f"提取器启动失败：{error_text}")
        self._extractor_process = None
        process.deleteLater()

    def _cancel(self):
        self._cancelled = True
        self.cancel_button.setEnabled(False)
        self.status_label.setText("正在取消…")
        process = self._extractor_process
        if is_safe(process):
            process.kill()

    def shutdown(self):
        for button in (
                getattr(self, "enable_shortcut_button", None),
                getattr(self, "edit_popup_button", None),
        ):
            if is_safe(button) and button._capturing:
                button._end_capture()
        self._cancelled = True
        process = self._extractor_process
        if is_safe(process):
            process.kill()
            process.waitForFinished(1500)
        self._extractor_process = None
        self._cleanup_extractor_request()


_label_extractor_dialog = None
_label_extractor_action = None
_label_extractor_menu_bar = None


def _set_layers_panel_translation(enabled):
    """Toggle C++ translation of UI controls inside Painter's Layers panel."""
    global TRANSLATE_LAYERS_PANEL
    TRANSLATE_LAYERS_PANEL = bool(enabled)
    QtCore.QSettings().setValue(
        "substance3d_chinese_translator/translate_layers_panel",
        TRANSLATE_LAYERS_PANEL,
    )
    _call_native(
        "sp_delegate_set_translate_layers", int(TRANSLATE_LAYERS_PANEL),
        label="切换图层面板翻译",
    )


def _sync_designer_graph_translation():
    """将节点翻译设置推送到原生引擎，返回是否已安全应用。"""
    if HOST != "designer":
        return False
    dll = _load_native_delegate()
    if dll is None:
        return False
    try:
        result = dll.sp_delegate_set_translate_designer_graph(
            int(TRANSLATE_DESIGNER_GRAPH)
        )
        if TRANSLATE_DESIGNER_GRAPH and result != 1:
            print(
                ">>> 节点翻译未启用：Qt 运行库不兼容，"
                "或绘制入口安装校验失败。"
            )
            return False
        if not TRANSLATE_DESIGNER_GRAPH and result != 1:
            print(">>> 警告：节点挂钩未能全部恢复，已切换为纯转发。")
        return True
    except Exception as exc:
        print(">>> 切换节点翻译失败：", exc, sep="")
        return False


def _set_designer_graph_translation(enabled):
    """只在用户明确确认后启用 Designer 节点绘制挂钩。"""
    global TRANSLATE_DESIGNER_GRAPH
    enabled = bool(enabled)
    dialog = (
        _label_extractor_dialog
        if is_safe(_label_extractor_dialog) else None
    )
    checkbox = (
        getattr(dialog, "graph_translation_check", None)
        if dialog is not None else None
    )
    if enabled and not TRANSLATE_DESIGNER_GRAPH:
        result = QtWidgets.QMessageBox.warning(
            dialog or _get_main_window(),
            "启用节点翻译",
            "节点标题和端口将通过显示层翻译，不修改节点数据。\n"
            "启用时存在未知风险，"
            "仍要启用吗？",
            QtWidgets.QMessageBox.Yes | QtWidgets.QMessageBox.No,
            QtWidgets.QMessageBox.No,
        )
        if result != QtWidgets.QMessageBox.Yes:
            if is_safe(checkbox):
                checkbox.blockSignals(True)
                checkbox.setChecked(False)
                checkbox.blockSignals(False)
            return
    TRANSLATE_DESIGNER_GRAPH = enabled
    QtCore.QSettings().setValue(
        "substance3d_chinese_translator/translate_designer_graph",
        TRANSLATE_DESIGNER_GRAPH,
    )
    if _sync_designer_graph_translation():
        return
    if enabled:
        TRANSLATE_DESIGNER_GRAPH = False
        QtCore.QSettings().setValue(
            "substance3d_chinese_translator/translate_designer_graph", False
        )
        if is_safe(checkbox):
            checkbox.blockSignals(True)
            checkbox.setChecked(False)
            checkbox.blockSignals(False)
        QtWidgets.QMessageBox.warning(
            dialog or _get_main_window(),
            "节点翻译未启用",
            "当前 Qt 运行库不兼容，"
            "或挂钩安装校验失败。\n"
            "普通界面翻译不受影响。",
        )


def _set_fuzzy_match(enabled):
    """Toggle the fuzzy fallback of the C++ translation engine.

    Exact dictionary lookups always win; fuzzy matching only catches casing,
    full/half-width, underscore and whitespace differences.
    """
    global FUZZY_MATCH_ENABLED
    FUZZY_MATCH_ENABLED = bool(enabled)
    QtCore.QSettings().setValue(
        "substance3d_chinese_translator/fuzzy_match", FUZZY_MATCH_ENABLED
    )
    _call_native(
        "sp_delegate_set_fuzzy_match", int(FUZZY_MATCH_ENABLED),
        label="切换模糊匹配",
    )


def _set_fallback_scan(enabled):
    """切换 C++ 的每 10 秒全量扫描兜底（默认关闭）。"""
    global FALLBACK_SCAN_ENABLED
    FALLBACK_SCAN_ENABLED = bool(enabled)
    QtCore.QSettings().setValue(
        "substance3d_chinese_translator/fallback_scan",
        FALLBACK_SCAN_ENABLED,
    )
    _call_native(
        "sp_delegate_set_fallback_scan", int(FALLBACK_SCAN_ENABLED),
        label="切换全量扫描兜底",
    )


def _toggle_enable_shortcut():
    """快捷键：切换面板里“启用插件翻译”复选框的勾选状态。"""
    if IS_APP_QUITTING or IS_CLEANING:
        return
    print(">>> [快捷键] 启用/禁用快捷键已触发")
    dialog = _label_extractor_dialog if is_safe(_label_extractor_dialog) else None
    if dialog is not None:
        dialog.translation_enabled_check.setChecked(
            not dialog.translation_enabled_check.isChecked()
        )
    else:
        _set_translation_enabled(not IS_TRANSLATION_ENABLED)


_shortcut_callback = None
_dictionary_reload_callback = None
@ctypes.CFUNCTYPE(ctypes.c_int)
def _on_native_dictionary_reload():
    """原生层删除词条后，立即复用启动时的词包加载与同步流程。"""
    try:
        load_translation_packages()
        return int(_sync_native_dictionary())
    except Exception as exc:
        print(">>> 原生编辑后的词库重载失败：", exc, sep="")
        return 0


def _apply_dictionary_reload_callback(dll):
    """注册一次持久回调，避免 C++ 端复制 Python 的词包合并规则。"""
    global _dictionary_reload_callback
    if _dictionary_reload_callback is None:
        _dictionary_reload_callback = _on_native_dictionary_reload
    dll.sp_delegate_set_dictionary_reload_callback(
        ctypes.cast(_dictionary_reload_callback, ctypes.c_void_p)
    )


@ctypes.CFUNCTYPE(None, ctypes.c_int)
def _on_shortcut(kind):
    """原生引擎识别到快捷键后的回调（0=启用/禁用）。"""
    if kind == 0:
        _toggle_enable_shortcut()


def _apply_shortcuts():
    """把快捷键配置与回调推送给原生引擎（识别与触发都在 C++）。"""
    global _shortcut_callback
    dll = _load_native_delegate()
    if dll is None:
        return
    try:
        if _shortcut_callback is None:
            _shortcut_callback = _on_shortcut
        dll.sp_delegate_set_shortcut_callback(
            ctypes.cast(_shortcut_callback, ctypes.c_void_p)
        )
        dll.sp_delegate_set_enable_shortcut(ENABLE_SHORTCUT or "")
    except Exception as exc:
        print(">>> 同步快捷键配置失败：", exc, sep="")


def _clear_native_shortcuts():
    """卸载时清空原生快捷键配置与回调，避免回调进入已卸载的模块。"""
    # 初次启动也会先走一次公共清理流程。此处不得为了“清理”而加载
    # 尚未存在的 DLL，否则随后启动引擎时会再次加载并重复打印构建标识。
    dll = _native_delegate
    if dll is None:
        return
    try:
        dll.sp_delegate_set_shortcut_callback(None)
        dll.sp_delegate_set_dictionary_reload_callback(None)
        dll.sp_delegate_set_enable_shortcut("")
    except Exception as exc:
        print(">>> 清理快捷键配置失败：", exc, sep="")


def _set_translation_enabled(enabled, persist=True):
    """Toggle the whole translation engine on/off.

    Unchecking the master switch stops translation and restores every
    translated widget in the interface back to its original text.
    """
    global IS_TRANSLATION_ENABLED
    IS_TRANSLATION_ENABLED = bool(enabled)
    if persist:
        if is_safe(_startup_timer):
            _startup_timer.stop()
        QtCore.QSettings().setValue(
            "substance3d_chinese_translator/enabled", IS_TRANSLATION_ENABLED
        )
    _call_native(
        "sp_delegate_set_enabled", int(IS_TRANSLATION_ENABLED),
        label="切换插件翻译总开关",
    )
    if (HOST == "designer" and IS_TRANSLATION_ENABLED
            and TRANSLATE_DESIGNER_GRAPH
            and not _sync_designer_graph_translation()):
        _set_designer_graph_translation(False)
    if is_safe(_label_extractor_dialog):
        try:
            _label_extractor_dialog.translation_enabled_check.setChecked(
                IS_TRANSLATION_ENABLED
            )
            _label_extractor_dialog._update_translation_controls()
        except Exception:
            pass


def _version_tuple(version):
    """Normalize ``v2.0.0`` / ``2.0`` / ``2.0.0-rc1`` to (major, minor, patch)."""
    text = str(version).strip().lstrip("vV")
    parts = []
    for part in re.split(r"[._-]", text):
        digits = re.match(r"\d+", part)
        if not digits:
            break
        parts.append(int(digits.group()))
        if len(parts) >= 3:
            break
    while len(parts) < 3:
        parts.append(0)
    return tuple(parts[:3])


def _http_get_json(url, timeout=15):
    parsed = urllib.parse.urlparse(url)
    if parsed.scheme != "https" or parsed.hostname != "api.github.com":
        raise RuntimeError("版本信息地址不是预期的 GitHub API。")
    request = urllib.request.Request(
        url, headers={"User-Agent": "substance3d_chinese_translator-updater"}
    )
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8"))


def _latest_release_info_api():
    """Query the GitHub Releases API for the newest official release."""
    data = _http_get_json(PLUGIN_RELEASE_URL)
    tag = (data.get("tag_name") or "").strip()
    if not tag:
        raise RuntimeError("GitHub 返回的发布信息缺少版本号。")
    version = tag.lstrip("vV")
    download_url = ""
    digest = ""
    for asset in data.get("assets") or []:
        if asset.get("name") == PLUGIN_ASSET_NAME:
            download_url = asset.get("browser_download_url") or ""
            digest = asset.get("digest") or ""
            break
    if not download_url:
        # Accept versioned names such as
        # substance3d_chinese_translator_2.0.1.zip.
        for asset in data.get("assets") or []:
            name = (asset.get("name") or "").strip()
            lowered = name.casefold()
            if lowered.startswith(
                "substance3d_chinese_translator"
            ) and lowered.endswith(".zip"):
                download_url = asset.get("browser_download_url") or ""
                digest = asset.get("digest") or ""
                break
    if not download_url:
        raise RuntimeError(
            f"最新发布 {tag} 中没有找到 {PLUGIN_ASSET_NAME} 安装包"
            "（支持 substance3d_chinese_translator.zip 或 "
            "substance3d_chinese_translator_版本.zip）。"
        )
    if not re.fullmatch(r"sha256:[0-9a-fA-F]{64}", digest):
        raise RuntimeError(
            "GitHub 发布信息缺少可验证的 SHA-256 摘要，"
            "为避免安装未经校验的更新，已中止。"
        )
    notes = data.get("body") or ""
    return version, download_url, notes, digest.split(":", 1)[1].lower()


def _latest_release_info():
    """获取最新正式版及 GitHub 生成的资产摘要。

    不再回退到网页重定向，因为该路径不提供可供客户端
    校验的发布资产摘要。
    """
    return _latest_release_info_api()


class _DownloadCancelled(Exception):
    pass


class _DownloadProgressDialog(QtWidgets.QDialog):
    """Modal download progress dialog with a cancel button.

    Built from plain widgets instead of QProgressDialog so the label, the
    progress bar and the cancel button always render inside Painter's dark
    theme.
    """

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowTitle("下载更新")
        self.setWindowModality(WINDOW_MODAL)
        self.setMinimumWidth(420)
        self.setMinimumHeight(140)
        layout = QtWidgets.QVBoxLayout(self)
        self._label = QtWidgets.QLabel("正在下载更新…", self)
        self._label.setWordWrap(True)
        layout.addWidget(self._label)
        self._bar = QtWidgets.QProgressBar(self)
        self._bar.setRange(0, 0)
        layout.addWidget(self._bar)
        self._cancel_button = QtWidgets.QPushButton("取消", self)
        self._cancel_button.setAutoDefault(False)
        align_right = (
            QtCore.Qt.AlignmentFlag.AlignRight
            if QT_MAJOR >= 6
            else QtCore.Qt.AlignRight
        )
        layout.addWidget(self._cancel_button, alignment=align_right)
        self._cancelled = False
        self._cancel_button.clicked.connect(self._request_cancel)

    def _request_cancel(self):
        self._cancelled = True
        self._cancel_button.setEnabled(False)
        self._label.setText("正在取消…")

    def reject(self):
        # The owned poll timer exits the modal loop promptly; the worker alone
        # owns its output file until it finishes.
        self._request_cancel()

    def set_progress(self, downloaded, total):
        if total > 0:
            self._bar.setRange(0, 100)
            self._bar.setValue(int(downloaded * 100.0 / total))
            self._label.setText(
                f"正在下载更新… "
                f"{downloaded // (1024 * 1024)} MB / "
                f"{total // (1024 * 1024)} MB"
            )
        else:
            self._bar.setRange(0, 0)

    def set_finished(self):
        self._bar.setRange(0, 100)
        self._bar.setValue(100)
        self._label.setText("下载完成")

    def is_cancelled(self):
        return self._cancelled


def _normalized_zip_name(info):
    """Return a safe, normalized archive member name or raise."""
    raw_name = info.filename.replace("\\", "/")
    parts = [part for part in raw_name.split("/") if part not in ("", ".")]
    unsafe = (
        not parts
        or raw_name.startswith("/")
        or any(part == ".." for part in parts)
        or (len(raw_name) >= 2 and raw_name[1] == ":")
        or any(ord(character) < 0x20 for character in raw_name)
        or any(":" in part or part.endswith((".", " ")) for part in parts)
        or ((info.external_attr >> 16) & 0o170000) == 0o120000
    )
    if unsafe:
        raise RuntimeError(f"更新包包含不安全路径或链接：{info.filename}")
    return "/".join(parts)


def _validate_update_archive(path, expected_version=None):
    """Validate package structure and bounded extraction cost."""
    with zipfile.ZipFile(path) as archive:
        infos = archive.infolist()
        if len(infos) > MAX_UPDATE_FILES:
            raise RuntimeError("更新包文件数超过安全上限。")
        file_names = set()
        folded_names = set()
        expanded = 0
        for info in infos:
            name = _normalized_zip_name(info)
            folded = name.casefold()
            if folded in folded_names:
                raise RuntimeError(f"更新包含重复路径：{name}")
            folded_names.add(folded)
            if info.is_dir():
                continue
            file_names.add(name)
            if info.flag_bits & 0x1:
                raise RuntimeError(f"更新包含加密文件：{name}")
            if info.file_size > MAX_UPDATE_FILE_BYTES:
                raise RuntimeError(f"更新包单个文件过大：{name}")
            expanded += info.file_size
            if expanded > MAX_UPDATE_EXPANDED_BYTES:
                raise RuntimeError("更新包解压总大小超过安全上限。")
            if (info.file_size > 1024 * 1024
                    and info.file_size >
                    max(1, info.compress_size) * MAX_UPDATE_COMPRESSION_RATIO):
                raise RuntimeError(f"更新包文件压缩比异常：{name}")
        missing = REQUIRED_UPDATE_FILES.difference(file_names)
        if missing:
            raise RuntimeError(
                f"下载的发布包缺少必要文件：{sorted(missing)}"
            )
        unexpected = file_names.difference(RELEASE_FILE_ALLOWLIST)
        if unexpected:
            raise RuntimeError(
                f"下载的发布包包含非白名单文件：{sorted(unexpected)}"
            )
        try:
            metadata = json.loads(archive.read("pluginInfo.json").decode("utf-8-sig"))
        except Exception as exc:
            raise RuntimeError(f"更新包 pluginInfo.json 无效：{exc}")
        if metadata.get("name") != "substance3d_chinese_translator":
            raise RuntimeError("更新包插件标识不匹配。")
        packaged_version = str(metadata.get("version") or "").lstrip("vV")
        if expected_version and packaged_version != str(expected_version).lstrip("vV"):
            raise RuntimeError(
                f"更新包版本 {packaged_version!r} 与发布版本 "
                f"{expected_version!r} 不一致。"
            )
        bad_member = archive.testzip()
        if bad_member:
            raise RuntimeError(f"更新包校验失败：{bad_member}")


def _download_update(url, destination, expected_sha256, expected_version,
                     progress=None, is_cancelled=None):
    """Download the release ZIP and verify it is a complete plug-in package.

    ``progress(downloaded_bytes, total_bytes)`` is invoked as data arrives;
    ``is_cancelled()`` is polled between chunks and may raise a
    ``_DownloadCancelled`` error through the download loop.
    """
    parsed = urllib.parse.urlparse(url)
    expected_prefix = f"/{PLUGIN_REPO}/releases/download/"
    if (parsed.scheme != "https" or parsed.hostname != "github.com"
            or not parsed.path.startswith(expected_prefix)):
        raise RuntimeError("更新下载地址不是预期的 GitHub Release 资产。")
    if not re.fullmatch(r"[0-9a-f]{64}", expected_sha256 or ""):
        raise RuntimeError("更新包缺少有效的 SHA-256 摘要。")
    request = urllib.request.Request(
        url, headers={"User-Agent": "substance3d_chinese_translator-updater"}
    )
    digest = hashlib.sha256()
    with urllib.request.urlopen(request, timeout=15) as response:
        final_url = urllib.parse.urlparse(response.geturl())
        if (final_url.scheme != "https" or final_url.hostname not in {
                "github.com", "objects.githubusercontent.com",
                "release-assets.githubusercontent.com"}):
            raise RuntimeError("更新下载重定向到了非 GitHub 资产地址。")
        try:
            total_bytes = int(response.headers.get("Content-Length") or 0)
        except (TypeError, ValueError):
            total_bytes = 0
        if total_bytes > MAX_UPDATE_DOWNLOAD_BYTES:
            raise RuntimeError("更新包大小超过安全上限。")
        with open(destination, "wb") as stream:
            downloaded = 0
            while True:
                if is_cancelled is not None and is_cancelled():
                    raise _DownloadCancelled("下载已取消")
                chunk = response.read(256 * 1024)
                if not chunk:
                    break
                stream.write(chunk)
                digest.update(chunk)
                downloaded += len(chunk)
                if downloaded > MAX_UPDATE_DOWNLOAD_BYTES:
                    raise RuntimeError("更新包大小超过安全上限。")
                if progress is not None:
                    progress(downloaded, total_bytes)
    actual_sha256 = digest.hexdigest()
    if actual_sha256 != expected_sha256:
        raise RuntimeError("更新包 SHA-256 校验失败，已拒绝安装。")
    _validate_update_archive(destination, expected_version)
    return destination


_update_check_active = False


def _run_release_query(parent):
    """Run network I/O off the UI thread; cancellation never joins that thread."""
    dialog = _DownloadProgressDialog(parent)
    dialog.setWindowTitle("检查更新")
    dialog._label.setText("正在检查 GitHub 更新…")
    done = threading.Event()
    state = {}

    def query():
        # This closure owns no Qt objects and never calls back into the engine.
        try:
            state["result"] = _latest_release_info()
        except Exception as exc:
            state["error"] = exc
        finally:
            done.set()

    timer = QtCore.QTimer(dialog)
    timer.setInterval(50)

    def poll():
        if IS_CLEANING or IS_APP_QUITTING or dialog.is_cancelled() or done.is_set():
            dialog.accept()

    timer.timeout.connect(poll)
    worker = threading.Thread(target=query, name="translation-release-query", daemon=True)
    try:
        worker.start()
        timer.start()
        dialog.exec_()
        if (IS_CLEANING or IS_APP_QUITTING or not is_safe(dialog)
                or dialog.is_cancelled() or not done.is_set()):
            raise _DownloadCancelled()
        if "error" in state:
            raise state["error"]
        return state["result"]
    finally:
        if is_safe(timer):
            timer.stop()
        if is_safe(dialog):
            delete(dialog)


def _check_updates(parent=None):
    global _update_check_active
    if _update_check_active or IS_CLEANING or IS_APP_QUITTING:
        return
    _update_check_active = True
    try:
        _check_updates_once(parent)
    finally:
        _update_check_active = False


def _check_updates_once(parent=None):
    """Check GitHub for a newer release and download it when available."""
    if parent is None:
        parent = QtWidgets.QApplication.activeWindow()
    # Replacing the plug-in while an extraction is running would fail on the
    # locked extractor EXE. Ask the user to finish or cancel the extraction
    # first instead of letting the update fail and roll back mid-way.
    tool_dialog = (
        _label_extractor_dialog if is_safe(_label_extractor_dialog) else parent
    )
    if (is_safe(tool_dialog)
            and getattr(tool_dialog, "_extractor_process", None) is not None):
        QtWidgets.QMessageBox.warning(
            parent,
            "正在提取词条",
            "当前正在进行词条提取，请先等待提取完成或点击“取消”后再检查更新。",
        )
        return
    try:
        version, download_url, notes, expected_sha256 = _run_release_query(parent)
        if _version_tuple(version) <= _version_tuple(PLUGIN_VERSION):
            QtWidgets.QMessageBox.information(
                parent,
                "检查更新",
                f"当前已是最新版本 v{PLUGIN_VERSION}",
            )
            return
        preview = "\n".join(
            line for line in notes.splitlines() if line.strip()
        )[:300]
        message = (
            f"发现新版本 {version}（当前 {PLUGIN_VERSION}）。"
            + (f"\n\n更新说明：\n{preview}" if preview else "")
            + "\n\n是否下载安装包？"
        )
        result = QtWidgets.QMessageBox.question(
            parent,
            "发现新版本",
            message,
            QtWidgets.QMessageBox.Yes | QtWidgets.QMessageBox.No,
            QtWidgets.QMessageBox.Yes,
        )
        if result != QtWidgets.QMessageBox.Yes:
            return

        descriptor, destination = tempfile.mkstemp(
            prefix="substance3d_chinese_translator_update_",
            suffix=".zip",
            dir=os.path.dirname(PLUGIN_DIR),
        )
        os.close(descriptor)
        progress_dialog = _DownloadProgressDialog(parent)
        cancel_event = threading.Event()
        state = {"downloaded": 0, "total": 0, "done": False, "error": None}

        def _download_worker():
            try:
                _download_update(
                    download_url, destination, expected_sha256, version,
                    lambda downloaded, total: state.update(downloaded=downloaded, total=total),
                    cancel_event.is_set,
                )
            except _DownloadCancelled:
                state["error"] = "cancelled"
            except Exception as exc:
                state["error"] = str(exc)
            finally:
                # The worker owns the file until it closes. Cancellation or
                # host teardown never makes the UI wait in thread.join().
                state["done"] = True
                if cancel_event.is_set():
                    try:
                        os.remove(destination)
                    except OSError:
                        pass

        def _on_cancel():
            cancel_event.set()
            if is_safe(progress_dialog):
                progress_dialog.accept()

        progress_dialog._cancel_button.clicked.connect(_on_cancel)
        worker = threading.Thread(target=_download_worker, daemon=True)
        timer = QtCore.QTimer(progress_dialog)
        timer.setInterval(100)

        def _tick():
            if IS_CLEANING or IS_APP_QUITTING or not is_safe(progress_dialog):
                cancel_event.set()
                if is_safe(progress_dialog):
                    progress_dialog.accept()
                return
            if progress_dialog.is_cancelled():
                _on_cancel()
                return
            if state["done"]:
                if not state["error"]:
                    progress_dialog.set_finished()
                progress_dialog.accept()
                return
            progress_dialog.set_progress(state["downloaded"], state["total"])

        timer.timeout.connect(_tick)
        cancelled = True
        try:
            worker.start()
            timer.start()
            progress_dialog.exec_()
            cancelled = (IS_CLEANING or IS_APP_QUITTING or not is_safe(progress_dialog)
                         or progress_dialog.is_cancelled() or not state["done"])
        finally:
            if cancelled:
                cancel_event.set()
                if not worker.is_alive():
                    try:
                        os.remove(destination)
                    except OSError:
                        pass
            if is_safe(timer):
                timer.stop()
            if is_safe(progress_dialog):
                delete(progress_dialog)

        if cancelled:
            cancel_event.set()
        error = state.get("error")
        if cancelled or error:
            # If completion raced with cancellation, the worker may already
            # have passed its cleanup check. It no longer holds the file here.
            if state["done"]:
                try:
                    os.remove(destination)
                except OSError:
                    pass
            if cancelled:
                return
            raise RuntimeError(error)
        # Apply the package in place without closing the host application,
        # then ask the user to restart so the new files (and native DLL)
        # are loaded.
        _apply_update_now(destination, parent)
    except _DownloadCancelled:
        return
    except Exception as exc:
        if IS_CLEANING or IS_APP_QUITTING or (parent is not None and not is_safe(parent)):
            return
        detail = str(exc)
        if "403" in detail or "rate limit" in detail.casefold():
            hint = (
                "GitHub API 触发限流（未登录每小时 60 次）。\n"
                "请稍后再试。"
            )
        else:
            hint = "请确认网络可访问 GitHub，稍后再试。"
        QtWidgets.QMessageBox.warning(
            parent,
            "检查更新失败",
            f"无法获取最新版本：\n{exc}\n\n{hint}",
        )


def _copy_file_safely(source, target):
    """Copy ``source`` to ``target``, working around a locked native binary.

    A DLL mapped into the host application (or the standalone extractor EXE
    while an extraction is running) cannot be overwritten, but Windows allows
    it to be renamed, so the old file is moved aside and the new one is
    written under the original name. The running session keeps using the old
    image in memory; the new file is loaded at the next start.
    """
    try:
        shutil.copy2(source, target)
        return
    except PermissionError:
        lowered = target.lower()
        if not (lowered.endswith(".dll") or lowered.endswith(".exe")):
            raise
        moved = target + ".old"
        if os.path.isfile(moved):
            try:
                os.remove(moved)
            except OSError:
                pass
        os.rename(target, moved)
        shutil.copy2(source, target)


def _copytree_merge(source, target):
    """Copy a directory tree into a possibly existing target directory."""
    if not os.path.isdir(target):
        shutil.copytree(source, target)
        return
    for name in os.listdir(source):
        src = os.path.join(source, name)
        dst = os.path.join(target, name)
        if os.path.isdir(src) and not os.path.islink(src):
            _copytree_merge(src, dst)
        else:
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            _copy_file_safely(src, dst)


def _cleanup_pending_native_files():
    """Remove native DLLs/EXEs renamed aside by a previous live update."""
    try:
        if not os.path.isdir(NATIVE_DIR):
            return
        for name in os.listdir(NATIVE_DIR):
            lowered = name.lower()
            if lowered.endswith(".dll.old") or lowered.endswith(".exe.old"):
                try:
                    os.remove(os.path.join(NATIVE_DIR, name))
                except OSError:
                    pass
    except Exception:
        pass


def _apply_update_now(zip_path, parent=None):
    """Apply a downloaded release package in place, without closing the host.

    The old plug-in directory is backed up, every file shipped by the new
    package is copied over it (a loaded native DLL is renamed aside first),
    and standalone user translation JSON files are preserved. Dictionaries
    shipped by the release are replaced exactly by the release copies. The
    host is left running; the new code and DLL only take effect after a restart.
    """
    if not os.path.isfile(zip_path):
        QtWidgets.QMessageBox.warning(
            parent,
            "无法自动更新",
            f"找不到已下载的更新包：\n{zip_path}\n请重新下载。",
        )
        return False

    backup_dir = UPDATE_BACKUP_DIR
    stage_dir = None
    preserve_dir = None
    replacement_started = False
    wait_cursor_active = False
    QtWidgets.QApplication.setOverrideCursor(WAIT_CURSOR)
    wait_cursor_active = True

    def restore_wait_cursor():
        nonlocal wait_cursor_active
        if wait_cursor_active:
            QtWidgets.QApplication.restoreOverrideCursor()
            wait_cursor_active = False

    try:
        stage_dir = tempfile.mkdtemp(prefix="sp_update_stage_")
        _validate_update_archive(zip_path)
        with zipfile.ZipFile(zip_path) as archive:
            # Use the same member validator as the preflight check.
            for info in archive.infolist():
                normalized = _normalized_zip_name(info)
                target = os.path.abspath(os.path.join(
                    stage_dir, *normalized.split("/")
                ))
                if os.path.commonpath((stage_dir, target)) != stage_dir:
                    raise RuntimeError(
                        f"更新包成员越过了暂存目录：{info.filename}"
                    )
                if info.is_dir():
                    os.makedirs(target, exist_ok=True)
                    continue
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with archive.open(info) as source, open(target, "wb") as output:
                    shutil.copyfileobj(source, output, length=256 * 1024)
        if not os.path.isfile(os.path.join(stage_dir, "__init__.py")):
            raise RuntimeError("更新包缺少 __init__.py，已中止更新。")

        if os.path.isdir(backup_dir):
            shutil.rmtree(backup_dir, ignore_errors=True)
        shutil.copytree(PLUGIN_DIR, backup_dir)

        # Preserve standalone user dictionaries. Release-owned dictionaries
        # are intentionally excluded so updates replace them exactly.
        preserve_dir = tempfile.mkdtemp(prefix="sp_update_preserve_")
        old_translations = os.path.join(PLUGIN_DIR, "translations")
        if os.path.isdir(old_translations):
            for name in os.listdir(old_translations):
                if (name.lower().endswith(".json")
                        and name.casefold() not in RELEASE_TRANSLATION_NAMES):
                    shutil.copy2(
                        os.path.join(old_translations, name),
                        os.path.join(preserve_dir, name),
                    )

        # Collect the file list shipped by the new package.
        new_files = set()
        for root, _dirs, files in os.walk(stage_dir):
            for name in files:
                rel = os.path.relpath(os.path.join(root, name), stage_dir)
                new_files.add(rel.replace(os.sep, "/"))

        # Replace every shipped file, tolerating the loaded native DLL.
        # Only failures after this point may roll back the fresh complete
        # backup. A rejected ZIP must never restore an unrelated old backup.
        replacement_started = True
        for rel in sorted(new_files):
            src = os.path.join(stage_dir, rel.replace("/", os.sep))
            target = os.path.join(PLUGIN_DIR, rel.replace("/", os.sep))
            os.makedirs(os.path.dirname(target), exist_ok=True)
            _copy_file_safely(src, target)

        # Remove stale files that no longer exist in the package, except for
        # the user's translation JSON files that are restored below.
        stale = []
        for root, _dirs, files in os.walk(PLUGIN_DIR):
            for name in files:
                rel = os.path.relpath(os.path.join(root, name), PLUGIN_DIR)
                rel_norm = rel.replace(os.sep, "/")
                if rel_norm in new_files:
                    continue
                if (
                    rel_norm.startswith("translations/")
                    and name.lower().endswith(".json")
                ):
                    continue
                stale.append((root, name))
        for root, name in stale:
            try:
                os.remove(os.path.join(root, name))
            except PermissionError:
                lowered = name.lower()
                if not (lowered.endswith(".dll.old")
                        or lowered.endswith(".exe.old")):
                    raise
            except OSError:
                pass

        # Restore standalone user dictionaries unchanged. Release-owned
        # dictionaries remain the exact files copied from the update package.
        new_translations = os.path.join(PLUGIN_DIR, "translations")
        os.makedirs(new_translations, exist_ok=True)
        for name in os.listdir(preserve_dir):
            target = os.path.join(new_translations, name)
            preserved = os.path.join(preserve_dir, name)
            shutil.copy2(preserved, target)

        if not os.path.isfile(os.path.join(PLUGIN_DIR, "__init__.py")):
            raise RuntimeError("替换后插件目录缺少 __init__.py。")

        result_file = UPDATE_RESULT_FILE
        try:
            with open(result_file, "w", encoding="utf-8") as stream:
                stream.write("true\n更新已应用，本次启动已加载新版本。")
        except OSError:
            pass

        # The package has been applied; do not leave the downloaded ZIP
        # behind in the plug-ins folder. On failure it is kept for retry.
        try:
            os.remove(zip_path)
        except OSError:
            pass

        # Result dialogs must use the normal pointer. Keeping WaitCursor until
        # the function-level finally block makes the cursor spin for the whole
        # time this modal prompt is open.
        restore_wait_cursor()
        QtWidgets.QMessageBox.information(
            parent,
            "更新已应用",
            "新版本文件已写入插件目录。\n\n"
            f"请重启 {HOST_DISPLAY_NAME} 以启用新版本。\n"
            "当前会话继续使用旧版本，不会自动关闭。",
        )
        return True
    except Exception as exc:
        # Roll back to the backup so a failed update never leaves a broken
        # plug-in directory.
        rollback_error = None
        try:
            if replacement_started and os.path.isdir(backup_dir) and os.path.isfile(
                os.path.join(backup_dir, "__init__.py")
            ):
                for name in os.listdir(PLUGIN_DIR):
                    path = os.path.join(PLUGIN_DIR, name)
                    try:
                        if os.path.isdir(path) and not os.path.islink(path):
                            shutil.rmtree(path, ignore_errors=True)
                        else:
                            os.remove(path)
                    except OSError:
                        pass
                _copytree_merge(backup_dir, PLUGIN_DIR)
        except Exception as restore_error:
            rollback_error = str(restore_error)
        kept = (
            f"\n更新包保留在：{zip_path}，可稍后重试或手动安装。"
            if os.path.isfile(zip_path) else ""
        )
        restore_wait_cursor()
        recovery = ("插件目录未开始替换。" if not replacement_started else
                    "插件已恢复为更新前的版本。")
        if rollback_error:
            recovery = "自动恢复未完成：%s\n备份保留在：%s" % (rollback_error, backup_dir)
        QtWidgets.QMessageBox.warning(
            parent,
            "更新失败",
            f"应用更新失败：\n{exc}\n\n{recovery}{kept}",
        )
        return False
    finally:
        restore_wait_cursor()
        for path in (stage_dir, preserve_dir):
            if path and os.path.isdir(path):
                shutil.rmtree(path, ignore_errors=True)


def _cleanup_update_remnants():
    """Clean only this installation; another SP/SD instance may be updating."""
    if os.path.isdir(UPDATE_BACKUP_DIR):
        shutil.rmtree(UPDATE_BACKUP_DIR, ignore_errors=True)
    try:
        os.remove(UPDATE_RESULT_FILE)
    except OSError:
        pass


def _notify_update_result(main_window):
    """Show the outcome of a background auto-update on the next start."""
    result_file = UPDATE_RESULT_FILE
    if not os.path.isfile(result_file):
        return
    message = None
    warning = False
    update_ok = False
    try:
        with open(result_file, encoding="utf-8-sig") as stream:
            lines = stream.read().splitlines()
        if lines and lines[0].strip() == "true":
            message = "插件已更新成功。\n\n" + "\n".join(lines[1:])
            update_ok = True
        elif lines:
            message = "自动更新未完成：\n" + "\n".join(lines[1:])
            warning = True
    except Exception:
        message = None
    try:
        os.remove(result_file)
    except OSError:
        pass
    if update_ok:
        # The new version is running, so the backup and temporary leftovers
        # are removed before the success prompt is displayed.
        _cleanup_update_remnants()
    # Already called by the owned startup notification timer; a second detached
    # singleShot could outlive plugin teardown.
    if message and is_safe(main_window) and not IS_CLEANING and not IS_APP_QUITTING:
        show = QtWidgets.QMessageBox.warning if warning else QtWidgets.QMessageBox.information
        show(main_window, "更新未完成" if warning else "插件已更新", message)


def show_translation_tool():
    global _label_extractor_dialog
    if not is_safe(_label_extractor_dialog):
        _label_extractor_dialog = ChineseTranslationToolDialog(
            _get_main_window()
        )
    _label_extractor_dialog.fuzzy_match_check.setChecked(FUZZY_MATCH_ENABLED)
    _label_extractor_dialog.fallback_scan_check.setChecked(
        FALLBACK_SCAN_ENABLED
    )
    _label_extractor_dialog.enable_shortcut_button.set_value(
        ENABLE_SHORTCUT
    )
    _label_extractor_dialog.edit_popup_button.set_value(
        EDIT_TRIGGER.display_text()
    )
    _label_extractor_dialog.translation_enabled_check.setChecked(
        IS_TRANSLATION_ENABLED
    )
    if (HOST == "designer"
            and hasattr(_label_extractor_dialog, "graph_translation_check")):
        _label_extractor_dialog.graph_translation_check.setChecked(
            TRANSLATE_DESIGNER_GRAPH
        )
    _label_extractor_dialog._update_translation_controls()
    _label_extractor_dialog.show()
    _label_extractor_dialog.raise_()
    _label_extractor_dialog.activateWindow()
    _apply_shortcuts()


# ==========================================
# 9. 生命周期的启动与清理
# ==========================================
def _disable_native_engine():
    """停止原生翻译引擎并恢复界面原文。"""
    if _native_delegate is not None:
        try:
            if HOST == "designer":
                _native_delegate.sp_delegate_set_translate_designer_graph(0)
            _native_delegate.sp_delegate_set_enabled(0)
        except Exception:
            pass


def _destroy_tool_dialog():
    """销毁翻译工具面板（若存在）。"""
    global _label_extractor_dialog
    if is_safe(_label_extractor_dialog):
        try:
            _label_extractor_dialog.shutdown()
            _label_extractor_dialog.close()
            delete(_label_extractor_dialog)
        except Exception:
            pass
    _label_extractor_dialog = None


def _remove_menu_action(action, menu_bar=None):
    """从宿主菜单栏移除并删除菜单动作。"""
    if not is_safe(action):
        return
    try:
        if not is_safe(menu_bar):
            main_window = _get_main_window()
            if is_safe(main_window):
                menu_bar = main_window.menuBar()
        if is_safe(menu_bar):
            menu_bar.removeAction(action)
        delete(action)
    except Exception:
        pass


def _teardown_engine():
    """卸载时的公共清理：原生快捷键、原生引擎、工具面板。"""
    global IS_APP_QUITTING, IS_CLEANING, _update_notification_timer
    IS_APP_QUITTING = True
    IS_CLEANING = True
    if is_safe(_update_notification_timer):
        _update_notification_timer.stop()
        delete(_update_notification_timer)
    _update_notification_timer = None
    _clear_native_shortcuts()
    _disable_native_engine()
    _uninstall_native_ui()
    _release_native_delegate()
    _destroy_tool_dialog()


def _start_native_engine():
    """加载词库并启动原生引擎，返回 (词库是否OK, UI是否OK, json耗时, 同步耗时, UI耗时)。"""
    global TRANSLATE_DESIGNER_GRAPH
    _cleanup_pending_native_files()
    phase = time.perf_counter()
    load_translation_packages()
    json_ms = (time.perf_counter() - phase) * 1000.0
    phase = time.perf_counter()
    dictionary_ok = _sync_native_dictionary()
    sync_ms = (time.perf_counter() - phase) * 1000.0
    phase = time.perf_counter()
    ui_ok = _install_native_ui(QtWidgets.QApplication.instance())
    if HOST == "designer" and TRANSLATE_DESIGNER_GRAPH:
        if not _sync_designer_graph_translation():
            TRANSLATE_DESIGNER_GRAPH = False
            QtCore.QSettings().setValue(
                "substance3d_chinese_translator/translate_designer_graph",
                False,
            )
    ui_ms = (time.perf_counter() - phase) * 1000.0
    if not dictionary_ok or not ui_ok:
        print(
            ">>> 原生翻译引擎未完全启用："
            f"词典同步={dictionary_ok}，界面安装={ui_ok}"
        )
    return dictionary_ok, ui_ok, json_ms, sync_ms, ui_ms


def _schedule_update_notification(main_window):
    """启动 2 秒后提示上次在线更新的结果。"""
    global _update_notification_timer
    if is_safe(_update_notification_timer):
        _update_notification_timer.stop()
        delete(_update_notification_timer)
    _update_notification_timer = QtCore.QTimer(main_window)
    _update_notification_timer.setSingleShot(True)
    _update_notification_timer.timeout.connect(
        lambda window=main_window: (
            _notify_update_result(window)
            if not IS_CLEANING and not IS_APP_QUITTING and is_safe(window) else None
        )
    )
    _update_notification_timer.start(2000)


def _register_tool_action(main_window):
    """在宿主菜单栏添加“中文翻译工具”入口并返回该动作。"""
    action = QAction("中文翻译工具", main_window)
    action.setObjectName("substance3d_chinese_translator_tool_action")
    action.triggered.connect(show_translation_tool)
    main_window.menuBar().addAction(action)
    return action


def close_plugin():
    global IS_APP_QUITTING, IS_CLEANING, IS_TRANSLATION_ENABLED
    global _label_extractor_action, _label_extractor_menu_bar

    if IS_CLEANING:
        return

    IS_CLEANING = True
    IS_APP_QUITTING = True
    IS_TRANSLATION_ENABLED = False

    _teardown_engine()
    _remove_menu_action(_label_extractor_action, _label_extractor_menu_bar)
    _label_extractor_action = None
    _label_extractor_menu_bar = None


# ==========================================
# 9b. Designer 生命周期的启动与清理
# ==========================================
def initializeSDPlugin():
    """Substance 3D Designer 插件入口。"""
    global _tool_action, _startup_timer
    global IS_APP_QUITTING, IS_CLEANING, IS_TRANSLATION_ENABLED

    if is_safe(_tool_action):
        return

    main_window = _get_main_window()
    if not is_safe(main_window):
        raise RuntimeError("Designer 的 Qt 界面管理器不可用")

    IS_APP_QUITTING = False
    IS_CLEANING = False
    _load_saved_settings()

    startup_enabled = IS_TRANSLATION_ENABLED
    IS_TRANSLATION_ENABLED = False
    _start_native_engine()
    # Temporary startup suppression must not overwrite the saved preference.

    # Designer 菜单栏只保留一个“中文翻译工具”入口；翻译总开关在工具窗口内。
    _tool_action = _register_tool_action(main_window)

    _startup_timer = QtCore.QTimer(main_window)
    _startup_timer.setSingleShot(True)
    _startup_timer.timeout.connect(
        lambda: _set_translation_enabled(startup_enabled, persist=False)
    )
    _startup_timer.start(3000)
    _apply_shortcuts()
    _schedule_update_notification(main_window)
    print("[Designer 中文翻译] 插件已启动，版本", PLUGIN_VERSION)


def uninitializeSDPlugin():
    """Substance 3D Designer 插件卸载入口。"""
    global _tool_action, _startup_timer

    if is_safe(_startup_timer):
        try:
            _startup_timer.stop()
            delete(_startup_timer)
        except Exception:
            pass
    _teardown_engine()
    _remove_menu_action(_tool_action)
    _tool_action = None
    _startup_timer = None
    print("[Designer 中文翻译] 插件已停止")


def _set_registered_plugin_display_name(main_window):
    """Rename this plugin's existing Painter Python-menu registration."""
    if not is_safe(main_window):
        return False
    module_name = __name__.split(".")[-1]
    menu_bar = main_window.menuBar()
    if not is_safe(menu_bar):
        return False
    for top_action in menu_bar.actions():
        menu = top_action.menu()
        if not is_safe(menu):
            continue
        for action in menu.actions():
            if action.text().replace("&", "") == module_name:
                action.setText(PLUGIN_DISPLAY_NAME)
                action.setObjectName(
                    "substance3d_chinese_translator_plugin_registration"
                )
                return True
    return False


def start_plugin():
    global IS_APP_QUITTING, IS_CLEANING
    global _label_extractor_action, _label_extractor_menu_bar

    # Painter 在启动过程中可能重复调用插件入口（已完整启动过就直接跳过），
    # 避免词库被重复加载、菜单被重复注册。
    if (not IS_CLEANING and _native_delegate is not None
            and _label_extractor_action is not None):
        return

    app = QtWidgets.QApplication.instance()
    if not is_safe(app):
        return

    startup_started = time.perf_counter()
    close_plugin()

    IS_APP_QUITTING = False
    IS_CLEANING = False
    _load_saved_settings()

    _, _, json_load_ms, native_sync_ms, native_ui_ms = _start_native_engine()
    if not IS_TRANSLATION_ENABLED:
        _set_translation_enabled(False)

    main_window = _get_main_window()
    if is_safe(main_window):
        # Painter starts this plugin while it is still populating the Python
        # menu. Renaming synchronously re-enters that construction and can
        # invalidate Painter's insertion separator, so wait until the menu
        # build returns.
        QtCore.QTimer.singleShot(
            0,
            lambda window=main_window: (
                _set_registered_plugin_display_name(window)
                if not IS_CLEANING and not IS_APP_QUITTING else None
            ),
        )
        _schedule_update_notification(main_window)
        _label_extractor_action = _register_tool_action(main_window)
        _label_extractor_menu_bar = main_window.menuBar()
    else:
        print(">>> 未获取到主窗口，跳过翻译工具菜单注册")
    _apply_shortcuts()

    total_ms = (time.perf_counter() - startup_started) * 1000.0
    print(
        ">>> 翻译插件启动完成："
        f"词条数：{len(TRANSLATE_DICT)}，"
        f"词典加载：{json_load_ms:.1f} 毫秒，"
        f"原生词典同步：{native_sync_ms:.1f} 毫秒，"
        f"原生界面安装：{native_ui_ms:.1f} 毫秒，"
        f"总耗时：{total_ms:.1f} 毫秒，"
        f"运行引擎：{'Qt6/C++' if QT_MAJOR >= 6 else 'Qt5/C++'}"
    )

    # Do not attach a module-level Python callback to aboutToQuit. Painter may
    # unload plugin modules before QApplication emits/destroys all Qt objects,
    # leaving a stale callback behind. The timer and filter are parented to the
    # application and close_plugin() is Painter's supported unload hook.

    # From this point on Python never reads, writes, or paints Painter-owned
    # controls. The native engine owns widget events, item
    # delegates, hover originals, dynamic refresh, and resource-tree painting.


def reload_plugin():
    """Painter 的“重新加载插件”入口：先完整清理，再重新启动。"""
    close_plugin()
    start_plugin()
