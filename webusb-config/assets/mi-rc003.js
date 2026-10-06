/*
 * MI-RC003 Bridge · WebUSB client library
 * =======================================
 * A dependency-free browser client for the MI-RC003 Bridge firmware. It wraps
 * the vendor WebUSB protocol in a small, readable API so a third-party UI only
 * needs to load this file and talk to the global `MiRC003` object.
 *
 * Quick start
 * -----------
 *   const dev = new MiRC003();
 *   await dev.connect();                 // the user picks the device
 *   console.log(await dev.status());
 *
 *   const keymap = await dev.getKeymap();
 *   MiRC003.Keymap.setGesture(keymap.layers[0], 0x28, "click",
 *                             MiRC003.Keymap.action(MiRC003.ACTIONS.KEYBOARD_TAP, { keyCode: 0x28 }));
 *   await dev.saveKeymap(keymap);        // chunked upload + persist
 *
 * Layout
 * ------
 *   1. Constants   – protocol, actions, physical keys, HID usage tables
 *   2. Keymap      – pure helpers to build/inspect the keymap JSON
 *   3. MiRC003     – the WebUSB client (connection, transport, commands)
 *
 * The full reference lives in api.md next to this file.
 */
(function (global) {
  "use strict";

  /* ==========================================================================
   * 1. Constants
   * ======================================================================== */

  /** USB identifiers the firmware advertises. */
  var DEFAULT_VID = 0x303a;
  var DEFAULT_PID = 0x8304;

  /** Vendor frame markers ('M' 'R') and header size. */
  var SOF0 = 0x4d;
  var SOF1 = 0x52;
  var HEADER_LEN = 6;
  var READ_CHUNK = 64;

  /** Command opcodes. Must match main/webusb/webusb_protocol.h. */
  var CMD = {
    DEVICE_INFO: 0x50,
    STATUS: 0x01,
    LOGS_GET: 0x02,
    LOGS_CLEAR: 0x03,
    KEYMAP_GET: 0x10,
    KEYMAP_SAVE: 0x11,
    KEYMAP_RESET: 0x12,
    KEYMAP_TELEMETRY: 0x13,
    KEYMAP_BEGIN: 0x15,
    KEYMAP_DATA: 0x16,
    KEYMAP_COMMIT: 0x17,
    SET_LAYER: 0x18,
    BLE_SCAN: 0x20,
    BLE_CONNECT: 0x21,
    BLE_UNPAIR: 0x22,
    BLE_INFO: 0x23,
    BLE_RECONNECT: 0x24,
    BLE_RAW_REPORT: 0x25,
    BLE_LAYOUT: 0x26,
    KEY_NAMES_GET: 0x27,
    KEY_NAMES_SET: 0x28,
    NVS_RESET: 0x31,
    SYSTEM_RESTART: 0x40,
  };

  /** Response status codes returned in the frame header. */
  var STATUS = {
    OK: 0,
    ERR_CMD: 1,
    ERR_ARG: 2,
    ERR_INTERNAL: 3,
  };

  /** Numeric action types (binding gestures). */
  var ACTIONS = {
    NONE: 0,
    KEYBOARD_TAP: 1,
    KEYBOARD_HOLD: 2,
    KEYBOARD_RELEASE: 3,
    CONSUMER_TAP: 4,
    CONSUMER_HOLD: 5,
    CONSUMER_RELEASE: 6,
    VOICE: 7,
    VOICE_RELEASE: 8,
    SWITCH_LAYER: 9,
    TRANSPARENT: 10,
    MOUSE_BUTTON_TAP: 11,
    MOUSE_BUTTON_HOLD: 12,
    MOUSE_BUTTON_RELEASE: 13,
    MOUSE_MOVE: 14,
    MOUSE_WHEEL: 15,
    ENTER_SWITCH_MODE: 16,
  };

  /** Human-readable action names, keyed by action type. */
  var ACTION = {
    0: "无",
    1: "键盘-单击",
    2: "键盘-按住",
    3: "键盘-释放",
    4: "多媒体-单击",
    5: "多媒体-按住",
    6: "多媒体-释放",
    7: "语音",
    8: "语音释放",
    9: "切换配置（旧）",
    10: "穿透继承",
    11: "鼠标按键-单击",
    12: "鼠标按键-按住",
    13: "鼠标按键-释放",
    14: "鼠标移动",
    15: "鼠标滚轮",
    16: "进入配置切换模式",
  };

  /** Gesture keys used by Keymap helpers. */
  var GESTURES = { CLICK: "click", LONG: "long", DOUBLE: "double", REPEAT: "repeat" };

  /**
   * Physical remote keys (canonical `source_vk` values).
   *
   * The array index **is** the physical slot: the firmware resolves whatever a
   * remote reports into one of these canonical key codes, and the code's
   * position here is the slot it lands in. Nothing downstream guesses what a
   * button *means* any more - the slot is the identity, and both the display
   * name and the emitted action are user-editable.
   *
   * This array is **not** an index table for any remote. A remote's raw report
   * index is only unique *within a single HID report*: the Google TV Remote
   * (ZTKA-IR57) exposes two report characteristics that each count from 1
   * (main 0x0021: 1 Up, 2 Down, ...; secondary 0x001D: 1 Volume+, 2 Volume-),
   * so "index N -> slot N-1" holds for neither report. The firmware maps the
   * pair (report, index) to a canonical code instead, which is why "index 1"
   * can be either the D-pad Up (slot 2) or Volume+ (slot 10). To see the raw
   * numbers, enable `raw report` and read the device log line
   * `HOGP: button chr#N idx=M -> key 0xXX (name) DOWN`.
   *
   * The order below is therefore only the historical default guess (the standard
   * Google TV remote button order: index 1 Power | 2 Assistant | 3-6 D-pad |
   * 7 Select | 8 Back | 9 Home | 10 Menu | 11-12 Volume | 13 Input, then Mute
   * and the two preset-app buttons that only some variants carry). On a remote
   * whose reports differ, the built-in label for a slot will not match the
   * physical button - that is expected, and the fix is to rename the slot in the
   * UI ("按键学习" tab) rather than to reorder this array.
   */
  var PHYSICAL_KEYS = [
    { vk: 0x66, name: "电源键" },
    { vk: 0x04, name: "语音键 / 助手" },
    { vk: 0x52, name: "方向上" },
    { vk: 0x51, name: "方向下" },
    { vk: 0x50, name: "方向左" },
    { vk: 0x4f, name: "方向右" },
    { vk: 0x28, name: "确定键" },
    { vk: 0xf1, name: "返回键" },
    { vk: 0x24, name: "主页键" },
    { vk: 0x5d, name: "菜单键" },
    { vk: 0x80, name: "音量+" },
    { vk: 0x81, name: "音量-" },
    { vk: 0xc0, name: "输入源键" },
    { vk: 0x7d, name: "静音键" },
    { vk: 0x7e, name: "预置应用 1" },
    { vk: 0x7f, name: "预置应用 2" },
  ];

  /** Number of physical slots the firmware exposes (== PHYSICAL_KEYS.length). */
  var KEY_SLOT_COUNT = PHYSICAL_KEYS.length;

  /**
   * UTF-8 byte budget for a custom key name, enforced by the firmware.
   *
   * It is a *byte* limit, not a character count: a Chinese name costs 3 bytes per
   * character, so only 10 of them fit. The firmware truncates blindly, which can
   * split a multi-byte character in half, so {@link MiRC003#setKeyName} truncates
   * on a character boundary first.
   */
  var KEY_NAME_MAX_BYTES = 31;

  /**
   * Frozen snapshot of the built-in labels, captured before any custom name is
   * applied, so the UI can show "default name" and restore it.
   *
   * Do not replace this with a live view of `PHYSICAL_KEYS[i].name`: the UI
   * writes user names into that field.
   */
  var DEFAULT_KEY_NAMES = PHYSICAL_KEYS.map(function (k) { return k.name; });

  /**
   * Factory default configuration-switch map. While the switch mode is active,
   * pressing the key selects the configuration in `layer`. Only the four
   * directions are user-editable; the confirm key is locked to the default
   * configuration by the firmware.
   */
  var SWITCH_MAP_DEFAULT = [
    { source_vk: 0x52, layer: 1 }, // 方向上 -> 配置 1
    { source_vk: 0x4f, layer: 2 }, // 方向右 -> 配置 2
    { source_vk: 0x51, layer: 3 }, // 方向下 -> 配置 3
    { source_vk: 0x50, layer: 4 }, // 方向左 -> 配置 4
  ];

  /** The confirm key is locked to the default configuration in switch mode. */
  var SWITCH_MAP_LOCKED = { source_vk: 0x28, layer: 0 };

  /** Keyboard modifier bitmasks (`*_mod`). */
  var MOD_BITS = [
    [0x01, "左Ctrl"], [0x02, "左Shift"], [0x04, "左Alt"], [0x08, "左Win"],
    [0x10, "右Ctrl"], [0x20, "右Shift"], [0x40, "右Alt"], [0x80, "右Win"],
  ];

  /** Mouse button bitmasks (`*_key` for mouse-button actions). */
  var MOUSE_BUTTONS = [
    [0x01, "左键"], [0x02, "右键"], [0x04, "中键"], [0x08, "后退键"], [0x10, "前进键"],
  ];

  /** Keyboard HID usages that appear on the on-screen 104-key layout. */
  var HID_GROUPS = [
    ["字母", [[0x04,"A"],[0x05,"B"],[0x06,"C"],[0x07,"D"],[0x08,"E"],[0x09,"F"],[0x0a,"G"],[0x0b,"H"],[0x0c,"I"],[0x0d,"J"],[0x0e,"K"],[0x0f,"L"],[0x10,"M"],[0x11,"N"],[0x12,"O"],[0x13,"P"],[0x14,"Q"],[0x15,"R"],[0x16,"S"],[0x17,"T"],[0x18,"U"],[0x19,"V"],[0x1a,"W"],[0x1b,"X"],[0x1c,"Y"],[0x1d,"Z"]]],
    ["数字", [[0x1e,"1 !"],[0x1f,"2 @"],[0x20,"3 #"],[0x21,"4 $"],[0x22,"5 %"],[0x23,"6 ^"],[0x24,"7 &"],[0x25,"8 *"],[0x26,"9 ("],[0x27,"0 )"]]],
    ["常用", [[0x28,"Enter 回车"],[0x29,"Esc"],[0x2a,"Backspace"],[0x2b,"Tab"],[0x2c,"Space 空格"],[0x39,"CapsLock"],[0x65,"Menu 菜单"]]],
    ["符号", [[0x2d,"- _"],[0x2e,"= +"],[0x2f,"[ {"],[0x30,"] }"],[0x31,"\\ |"],[0x33,"; :"],[0x34,"' \""],[0x35,"` ~"],[0x36,", <"],[0x37,". >"],[0x38,"/ ?"]]],
    ["功能键", [[0x3a,"F1"],[0x3b,"F2"],[0x3c,"F3"],[0x3d,"F4"],[0x3e,"F5"],[0x3f,"F6"],[0x40,"F7"],[0x41,"F8"],[0x42,"F9"],[0x43,"F10"],[0x44,"F11"],[0x45,"F12"],[0x68,"F13"],[0x69,"F14"],[0x6a,"F15"],[0x6b,"F16"],[0x6c,"F17"],[0x6d,"F18"],[0x6e,"F19"],[0x6f,"F20"],[0x70,"F21"],[0x71,"F22"],[0x72,"F23"],[0x73,"F24"]]],
    ["导航", [[0x49,"Insert"],[0x4a,"Home"],[0x4b,"PageUp"],[0x4c,"Delete"],[0x4d,"End"],[0x4e,"PageDown"],[0x4f,"方向→"],[0x50,"方向←"],[0x51,"方向↓"],[0x52,"方向↑"],[0x46,"PrintScreen"],[0x47,"ScrollLock"],[0x48,"Pause"]]],
    ["小键盘", [[0x53,"NumLock"],[0x54,"/"],[0x55,"*"],[0x56,"-"],[0x57,"+"],[0x58,"Num Enter"],[0x59,"1"],[0x5a,"2"],[0x5b,"3"],[0x5c,"4"],[0x5d,"5"],[0x5e,"6"],[0x5f,"7"],[0x60,"8"],[0x61,"9"],[0x62,"0"],[0x63,"."]]],
  ];

  /**
   * Common keyboard HID usages that are NOT on the on-screen layout but are
   * understood by Windows. Kept deliberately small and useful (F13+, a few
   * editing/application keys) for a "more keys" dropdown.
   */
  var HID_EXTRA_GROUPS = [
    ["功能键 (F13+)", [
      [0x68,"F13"],[0x69,"F14"],[0x6a,"F15"],[0x6b,"F16"],[0x6c,"F17"],[0x6d,"F18"],
      [0x6e,"F19"],[0x6f,"F20"],[0x70,"F21"],[0x71,"F22"],[0x72,"F23"],[0x73,"F24"],
    ]],
    ["编辑 / 应用", [
      [0x7a,"Undo 撤销"],[0x7b,"Cut 剪切"],[0x7c,"Copy 复制"],
      [0x7d,"Paste 粘贴"],[0x7e,"Find 查找"],
      [0x7f,"Mute 静音"],[0x80,"Volume Up 音量+"],[0x81,"Volume Down 音量-"],
    ]],
  ];

  /** Keyboard modifier HID usages (0xE0-0xE7) and their names. */
  var HID_MODIFIERS = {
    0xe0: "左Ctrl", 0xe1: "左Shift", 0xe2: "左Alt", 0xe3: "左Win",
    0xe4: "右Ctrl", 0xe5: "右Shift", 0xe6: "右Alt", 0xe7: "右Win",
  };

  /** Consumer (media/system) usages, grouped for dropdowns. */
  var CONSUMER_GROUPS = [
    ["媒体", [[0xcd,"播放/暂停"],[0xb5,"下一曲"],[0xb6,"上一曲"],[0xb7,"停止"],[0xb3,"快进"],[0xb4,"快退"]]],
    ["音量", [[0xe9,"音量+"],[0xea,"音量-"],[0xe2,"静音"]]],
    ["系统", [[0x30,"电源"],[0x32,"睡眠"],[0x183,"媒体选择"]]],
    ["浏览器", [[0x223,"浏览器主页"],[0x224,"浏览器返回"],[0x225,"浏览器前进"],[0x227,"浏览器刷新"],[0x221,"浏览器搜索"]]],
  ];

  /* ==========================================================================
   * 2. Keymap helpers (pure functions over the JSON structure)
   * ======================================================================== */

  var ACTION_SUFFIXES = ["mod", "key", "cons", "layer", "dx", "dy", "wheel"];

  function forEachItem(groups, fn) {
    for (var g = 0; g < groups.length; g++) {
      var items = groups[g][1];
      for (var i = 0; i < items.length; i++) fn(items[i][0], items[i][1], groups[g][0]);
    }
  }

  var Keymap = {
    GESTURES: GESTURES,

    /**
     * Create an action object accepted by {@link Keymap#setGesture}.
     * @param {number} type One of {@link ACTIONS}.
     * @param {object} [opts] { modifier, keyCode, consumerCode, targetLayer,
     *                          dx, dy, wheel, ms, delayMs, intervalMs }
     */
    action: function (type, opts) {
      opts = opts || {};
      return {
        type: type,
        modifier: opts.modifier || 0,
        keyCode: opts.keyCode || 0,
        consumerCode: opts.consumerCode || 0,
        targetLayer: opts.targetLayer || 0,
        dx: opts.dx || 0,
        dy: opts.dy || 0,
        wheel: opts.wheel || 0,
        ms: opts.ms,
        delayMs: opts.delayMs,
        intervalMs: opts.intervalMs,
      };
    },

    /** Deep clone a keymap object. */
    clone: function (keymap) {
      return JSON.parse(JSON.stringify(keymap));
    },

    /** Find a layer by id (0..4). Returns null when absent. */
    findLayer: function (keymap, id) {
      if (!keymap || !keymap.layers) return null;
      for (var i = 0; i < keymap.layers.length; i++) {
        if (keymap.layers[i].id === id) return keymap.layers[i];
      }
      return null;
    },

    /** Find the binding for a physical key inside a layer. */
    getBinding: function (layer, sourceVk) {
      if (!layer || !layer.bindings) return null;
      for (var i = 0; i < layer.bindings.length; i++) {
        if (layer.bindings[i].source_vk === sourceVk) return layer.bindings[i];
      }
      return null;
    },

    /** Get the binding for a key, creating an empty one when missing. */
    ensureBinding: function (layer, sourceVk) {
      if (!layer) return null;
      if (!layer.bindings) layer.bindings = [];
      var b = Keymap.getBinding(layer, sourceVk);
      if (!b) {
        b = { source_vk: sourceVk };
        layer.bindings.push(b);
      }
      return b;
    },

    /** Remove a key's binding entirely. Returns true when something changed. */
    removeBinding: function (layer, sourceVk) {
      if (!layer || !layer.bindings) return false;
      var before = layer.bindings.length;
      layer.bindings = layer.bindings.filter(function (b) { return b.source_vk !== sourceVk; });
      return layer.bindings.length !== before;
    },

    /** Global configuration-switch map (array of { source_vk, layer }). */
    getSwitchMap: function (keymap) {
      return (keymap && Array.isArray(keymap.switch_map)) ? keymap.switch_map : [];
    },

    /** Target configuration for a key in the switch map, or -1 when unmapped. */
    getSwitchTarget: function (keymap, sourceVk) {
      var map = Keymap.getSwitchMap(keymap);
      for (var i = 0; i < map.length; i++) {
        if (map[i].source_vk === sourceVk) return map[i].layer;
      }
      return -1;
    },

    /**
     * Bind a key to a target configuration (layer 0..4) in the switch map.
     * Passing a layer < 0 removes the key from the map. One key maps to one
     * configuration; several keys may target the same configuration.
     */
    setSwitchTarget: function (keymap, sourceVk, layer) {
      if (!keymap) return null;
      if (!Array.isArray(keymap.switch_map)) keymap.switch_map = [];
      var map = keymap.switch_map;
      var idx = -1;
      for (var i = 0; i < map.length; i++) {
        if (map[i].source_vk === sourceVk) { idx = i; break; }
      }
      if (layer == null || layer < 0) {
        if (idx >= 0) map.splice(idx, 1);
        return null;
      }
      if (idx >= 0) {
        map[idx].layer = layer;
        return map[idx];
      }
      var entry = { source_vk: sourceVk, layer: layer };
      map.push(entry);
      return entry;
    },

    /** Remove a key from the switch map. Returns true when something changed. */
    removeSwitchTarget: function (keymap, sourceVk) {
      if (!keymap || !Array.isArray(keymap.switch_map)) return false;
      var before = keymap.switch_map.length;
      keymap.switch_map = keymap.switch_map.filter(function (e) { return e.source_vk !== sourceVk; });
      return keymap.switch_map.length !== before;
    },

    /**
     * Set (or clear) one gesture of a key.
     * @param {object} layer    Layer object from the keymap.
     * @param {number} sourceVk Physical key code.
     * @param {string} gesture  "click" | "long" | "double" | "repeat".
     * @param {object|null} action Action from {@link Keymap#action}; null clears.
     * @returns {object} the updated binding.
     */
    setGesture: function (layer, sourceVk, gesture, action) {
      var b = Keymap.ensureBinding(layer, sourceVk);
      if (!b) return null;

      if (gesture === GESTURES.REPEAT) {
        if (!action || action.type === ACTIONS.NONE) {
          delete b.has_repeat;
          b.repeat_type = 0;
          ACTION_SUFFIXES.forEach(function (s) { delete b["repeat_" + s]; });
          delete b.repeat_delay_ms;
          delete b.repeat_interval_ms;
          return b;
        }
        b.has_repeat = true;
        writeActionFields(b, "repeat", action);
        b.repeat_delay_ms = action.delayMs != null ? action.delayMs : (b.repeat_delay_ms || 350);
        b.repeat_interval_ms = action.intervalMs != null ? action.intervalMs : (b.repeat_interval_ms || 70);
        return b;
      }

      if (gesture !== GESTURES.CLICK && gesture !== GESTURES.LONG && gesture !== GESTURES.DOUBLE) {
        throw new Error("未知手势: " + gesture);
      }

      if (!action || action.type === ACTIONS.NONE) {
        delete b["has_" + gesture];
        clearActionFields(b, gesture);
        return b;
      }

      b["has_" + gesture] = true;
      writeActionFields(b, gesture, action);
      if (gesture === GESTURES.LONG || gesture === GESTURES.DOUBLE) {
        b[gesture + "_ms"] = action.ms != null ? action.ms : (gesture === GESTURES.LONG ? 600 : 250);
      }
      return b;
    },

    /** Read one gesture back as an action object (or null). */
    getGesture: function (layer, sourceVk, gesture) {
      var b = Keymap.getBinding(layer, sourceVk);
      if (!b) return null;
      if (gesture === GESTURES.REPEAT) {
        if (!b.has_repeat) return null;
        return readActionFields(b, "repeat", { delayMs: b.repeat_delay_ms, intervalMs: b.repeat_interval_ms });
      }
      if (!b["has_" + gesture]) return null;
      var action = readActionFields(b, gesture, {});
      if (gesture === GESTURES.LONG || gesture === GESTURES.DOUBLE) action.ms = b[gesture + "_ms"];
      return action;
    },

    /** Human-readable summary of a key's mapping. */
    describe: function (layer, sourceVk) {
      var b = Keymap.getBinding(layer, sourceVk);
      if (!b) return "未配置";
      var parts = [];
      [GESTURES.CLICK, GESTURES.LONG, GESTURES.DOUBLE].forEach(function (g) {
        var a = Keymap.getGesture(layer, sourceVk, g);
        if (!a) return;
        var text = ACTION[a.type] || ("类型" + a.type);
        if (g !== GESTURES.CLICK) text = (g === GESTURES.LONG ? "长按 " : "双击 ") + text;
        parts.push(text);
      });
      if (b.has_repeat) parts.push("连发");
      return parts.length ? parts.join(" · ") : "未配置";
    },

    /** Name for a keyboard HID usage (searches both usage tables). */
    hidName: function (code) {
      if (HID_MODIFIERS[code]) return HID_MODIFIERS[code];
      var name = null;
      forEachItem(HID_GROUPS, function (v, n) { if (v === code) name = n; });
      if (name == null) forEachItem(HID_EXTRA_GROUPS, function (v, n) { if (v === code) name = n; });
      if (name != null) return name;
      return code ? "未知(0x" + code.toString(16).toUpperCase() + ")" : "未选择";
    },

    /**
     * Validate a keymap and return a list of human-readable problems.
     * An empty array means the keymap is structurally valid.
     */
    validate: function (keymap) {
      var errors = [];
      if (!keymap || typeof keymap !== "object") return ["keymap 不是对象"];
      if (!Array.isArray(keymap.layers)) return ["缺少 layers 数组"];
      keymap.layers.forEach(function (layer) {
        if (typeof layer.id !== "number") errors.push("配置缺少数字 id");
        if (!Array.isArray(layer.bindings)) errors.push("配置 " + layer.id + " 缺少 bindings 数组");
      });
      if (keymap.switch_map != null && !Array.isArray(keymap.switch_map)) {
        errors.push("switch_map 必须是数组");
      }
      return errors;
    },
  };

  function clearActionFields(b, prefix) {
    b[prefix + "_type"] = 0;
    ACTION_SUFFIXES.forEach(function (s) { delete b[prefix + "_" + s]; });
  }

  function writeActionFields(b, prefix, action) {
    clearActionFields(b, prefix);
    b[prefix + "_type"] = action.type;
    switch (action.type) {
      case ACTIONS.KEYBOARD_TAP:
      case ACTIONS.KEYBOARD_HOLD:
      case ACTIONS.VOICE:
        b[prefix + "_mod"] = action.modifier || 0;
        b[prefix + "_key"] = action.keyCode || 0;
        break;
      case ACTIONS.CONSUMER_TAP:
      case ACTIONS.CONSUMER_HOLD:
        b[prefix + "_cons"] = action.consumerCode || 0;
        break;
      case ACTIONS.SWITCH_LAYER:
        b[prefix + "_layer"] = action.targetLayer || 0;
        break;
      case ACTIONS.MOUSE_BUTTON_TAP:
      case ACTIONS.MOUSE_BUTTON_HOLD:
        b[prefix + "_key"] = action.keyCode || 0;
        break;
      case ACTIONS.MOUSE_MOVE:
        b[prefix + "_dx"] = action.dx || 0;
        b[prefix + "_dy"] = action.dy || 0;
        break;
      case ACTIONS.MOUSE_WHEEL:
        b[prefix + "_wheel"] = action.wheel || 0;
        break;
      default:
        break;
    }
  }

  function readActionFields(b, prefix, extra) {
    var action = {
      type: b[prefix + "_type"] || 0,
      modifier: b[prefix + "_mod"] || 0,
      keyCode: b[prefix + "_key"] || 0,
      consumerCode: b[prefix + "_cons"] || 0,
      targetLayer: b[prefix + "_layer"] || 0,
      dx: b[prefix + "_dx"] || 0,
      dy: b[prefix + "_dy"] || 0,
      wheel: b[prefix + "_wheel"] || 0,
    };
    if (extra) {
      if (extra.ms != null) action.ms = extra.ms;
      if (extra.delayMs != null) action.delayMs = extra.delayMs;
      if (extra.intervalMs != null) action.intervalMs = extra.intervalMs;
    }
    return action;
  }

  /* ==========================================================================
   * 3. MiRC003 client
   * ======================================================================== */

  /* ------------------------------ text helpers --------------------------- */

  var TEXT_ENCODER = new TextEncoder();

  /** Normalize a name table from the device into KEY_SLOT_COUNT strings. */
  function normalizeNames(list) {
    var out = [];
    for (var i = 0; i < KEY_SLOT_COUNT; i++) {
      out.push(list && typeof list[i] === "string" ? list[i] : "");
    }
    return out;
  }

  /**
   * Truncate a string so its UTF-8 encoding fits in `maxBytes`, without ever
   * splitting a character (a surrogate pair counts as one).
   *
   * The firmware enforces the same limit but cuts on a raw byte boundary, which
   * can leave a half-encoded character behind. Truncating here first means what
   * the user sees in the input is exactly what the device stores.
   */
  function truncateUtf8(str, maxBytes) {
    var s = String(str == null ? "" : str);
    var limit = maxBytes > 0 ? maxBytes : 0;
    var used = 0;
    var out = "";
    for (var i = 0; i < s.length; i++) {
      var cp = s.codePointAt(i);
      var ch = String.fromCodePoint(cp);
      if (cp > 0xffff) i++;  // surrogate pair: the character spans two code units
      var n = TEXT_ENCODER.encode(ch).length;
      if (used + n > limit) break;
      used += n;
      out += ch;
    }
    return out;
  }

  /**
   * @constructor
   * @param {object} [options]
   * @param {number} [options.vendorId=0x303a]
   * @param {number} [options.productId=0x8304]
   * @param {number} [options.saveChunk=64] Bytes per KEYMAP_DATA chunk.
   */
  function MiRC003(options) {
    options = options || {};
    this.vendorId = options.vendorId != null ? options.vendorId : DEFAULT_VID;
    this.productId = options.productId != null ? options.productId : DEFAULT_PID;
    this.saveChunk = options.saveChunk || 64;

    this._device = null;
    this._out = null;
    this._in = null;
    this._rx = new Uint8Array(0);
    this._chain = Promise.resolve();
    this._handlers = {};
    this._usbDisconnect = null;
  }

  /* ------------------------------ events --------------------------------- */

  /**
   * Subscribe to an event. Supported: "connect", "disconnect", "error".
   * @returns {MiRC003} this (chainable)
   */
  MiRC003.prototype.on = function (event, cb) {
    (this._handlers[event] = this._handlers[event] || []).push(cb);
    return this;
  };

  /** Remove a previously added handler. */
  MiRC003.prototype.off = function (event, cb) {
    var list = this._handlers[event];
    if (list) this._handlers[event] = list.filter(function (f) { return f !== cb; });
    return this;
  };

  MiRC003.prototype._emit = function (event, data) {
    var list = this._handlers[event] || [];
    for (var i = 0; i < list.length; i++) {
      try { list[i](data); } catch (e) { console.error(e); }
    }
  };

  /* ---------------------------- connection ------------------------------- */

  /** True while a device is open and the vendor interface is claimed. */
  MiRC003.prototype.isConnected = function () {
    return !!this._device;
  };

  /**
   * Ask the user to pick the device, open it and claim the vendor interface.
   * @returns {Promise<MiRC003>} resolves with this client.
   */
  MiRC003.prototype.connect = function () {
    var self = this;
    if (!global.navigator || !navigator.usb) {
      return Promise.reject(new Error("当前浏览器不支持 WebUSB"));
    }
    var out = null, inp = null, vendorItf = -1;

    return navigator.usb
      .requestDevice({ filters: [{ vendorId: self.vendorId, productId: self.productId }] })
      .then(function (device) {
        self._device = device;
        return device.open();
      })
      .then(function () {
        if (self._device.configuration === null) return self._device.selectConfiguration(1);
      })
      .then(function () {
        var itfs = self._device.configuration.interfaces;
        for (var i = 0; i < itfs.length; i++) {
          var alts = itfs[i].alternates;
          for (var j = 0; j < alts.length; j++) {
            if (alts[j].interfaceClass === 0xff) {
              vendorItf = itfs[i].interfaceNumber;
              out = alts[j].endpoints.find(function (e) { return e.direction === "out"; });
              inp = alts[j].endpoints.find(function (e) { return e.direction === "in"; });
              break;
            }
          }
          if (vendorItf >= 0) break;
        }
        if (vendorItf < 0 || !out || !inp) throw new Error("未找到 WebUSB 厂商接口");
        return self._device.claimInterface(vendorItf);
      })
      .then(function () {
        self._out = out;
        self._in = inp;
        self._rx = new Uint8Array(0);
        self._usbDisconnect = function (e) {
          if (e.device === self._device) {
            self._device = null; self._out = null; self._in = null;
            self._emit("disconnect");
          }
        };
        navigator.usb.addEventListener("disconnect", self._usbDisconnect);
        self._emit("connect", {
          outEndpoint: out.endpointNumber,
          inEndpoint: inp.endpointNumber,
        });
        return self;
      })
      .catch(function (err) {
        if (self._device) { try { self._device.close(); } catch (e) { /* ignore */ } }
        self._device = null; self._out = null; self._in = null;
        throw err;
      });
  };

  /** Close the device (no-op when not connected). */
  MiRC003.prototype.disconnect = function () {
    var self = this;
    var dev = this._device;
    this._device = null; this._out = null; this._in = null; this._rx = new Uint8Array(0);
    if (dev) {
      return dev.close().catch(function () {}).then(function () { self._emit("disconnect"); });
    }
    return Promise.resolve();
  };

  /* ------------------------- low-level transport ------------------------- */

  MiRC003.prototype._buildFrame = function (cmd, payload) {
    var len = payload ? payload.length : 0;
    var frame = new Uint8Array(HEADER_LEN + len);
    frame[0] = SOF0; frame[1] = SOF1; frame[2] = cmd; frame[3] = 0;
    frame[4] = len & 0xff; frame[5] = (len >> 8) & 0xff;
    if (len) frame.set(payload, HEADER_LEN);
    return frame;
  };

  MiRC003.prototype._readFrame = function () {
    var self = this;
    function pump() {
      if (self._rx.length >= HEADER_LEN) {
        if (self._rx[0] === SOF0 && self._rx[1] === SOF1) {
          var len = self._rx[4] | (self._rx[5] << 8);
          if (self._rx.length >= HEADER_LEN + len) {
            var result = {
              cmd: self._rx[2],
              status: self._rx[3],
              payload: self._rx.slice(HEADER_LEN, HEADER_LEN + len),
            };
            self._rx = self._rx.slice(HEADER_LEN + len);
            return Promise.resolve(result);
          }
        } else {
          self._rx = self._rx.slice(1);
          return pump();
        }
      }
      return self._device.transferIn(self._in.endpointNumber, READ_CHUNK).then(function (r) {
        if (r.status !== "ok" || !r.data) throw new Error("USB 读取失败: " + r.status);
        var chunk = new Uint8Array(r.data.buffer, r.data.byteOffset, r.data.byteLength);
        var merged = new Uint8Array(self._rx.length + chunk.length);
        merged.set(self._rx, 0);
        merged.set(chunk, self._rx.length);
        self._rx = merged;
        return pump();
      });
    }
    return pump();
  };

  /**
   * Send a raw command. All calls are serialized (one request in flight).
   * @param {number} cmd Command opcode.
   * @param {object|null} [payloadObj] JSON-serialized payload.
   * @param {Uint8Array|null} [rawBytes] Raw payload (takes precedence).
   * @returns {Promise<object>} parsed JSON response.
   */
  MiRC003.prototype.send = function (cmd, payloadObj, rawBytes) {
    var self = this;
    function run() { return self._send(cmd, payloadObj, rawBytes); }
    var p = this._chain.then(run, run);
    this._chain = p.catch(function () {});
    return p;
  };

  MiRC003.prototype._send = function (cmd, payloadObj, rawBytes) {
    if (!this._device || !this._out) return Promise.reject(new Error("设备未连接"));
    var payload = rawBytes
      ? rawBytes
      : (payloadObj ? new TextEncoder().encode(JSON.stringify(payloadObj)) : new Uint8Array(0));
    var frame = this._buildFrame(cmd, payload);
    return this._device
      .transferOut(this._out.endpointNumber, frame)
      .then(this._readFrame.bind(this))
      .then(function (resp) {
        if (resp.status !== STATUS.OK) throw new Error("设备返回错误 (status=" + resp.status + ")");
        var text = new TextDecoder().decode(resp.payload);
        return text ? JSON.parse(text) : {};
      });
  };

  /* --------------------------- high-level API ---------------------------- */

  /** Device identity: { name, version, build, hardware, protocol, capabilities[] }. */
  MiRC003.prototype.deviceInfo = function () { return this.send(CMD.DEVICE_INFO); };

  /** Runtime status snapshot (uptime, BLE state, memory, active layer, ...). */
  MiRC003.prototype.status = function () { return this.send(CMD.STATUS); };

  /** Latest key telemetry (pressed key, action, duration, active layer). */
  MiRC003.prototype.telemetry = function () { return this.send(CMD.KEYMAP_TELEMETRY); };

  /** Read the full multi-layer keymap. */
  MiRC003.prototype.getKeymap = function () { return this.send(CMD.KEYMAP_GET); };

  /** Restore factory default keymap. */
  MiRC003.prototype.resetKeymap = function () { return this.send(CMD.KEYMAP_RESET); };

  /** Switch the device's active configuration (layer index 0..4). */
  MiRC003.prototype.setLayer = function (layer) {
    return this.send(CMD.SET_LAYER, { layer: layer });
  };

  /**
   * Persist a keymap object (same shape as getKeymap()).
   * Uses KEYMAP_BEGIN/DATA/COMMIT so large keymaps stream in chunks.
   */
  MiRC003.prototype.saveKeymap = function (keymap) {
    var self = this;
    var json = new TextEncoder().encode(JSON.stringify(keymap));
    var step = this.saveChunk;
    return this.send(CMD.KEYMAP_BEGIN).then(function () {
      function next(off) {
        if (off >= json.length) return self.send(CMD.KEYMAP_COMMIT);
        return self.send(CMD.KEYMAP_DATA, null, json.subarray(off, off + step)).then(function () {
          return next(off + step);
        });
      }
      return next(0);
    });
  };

  /** Scan for nearby BLE remotes: { devices: [{ name, mac, rssi, type }] }. */
  MiRC003.prototype.bleScan = function () { return this.send(CMD.BLE_SCAN); };

  /** Connect to a scanned remote: target = { mac, type, name }. */
  MiRC003.prototype.bleConnect = function (target) { return this.send(CMD.BLE_CONNECT, target); };

  /** Forget the currently bound remote. */
  MiRC003.prototype.bleUnpair = function () { return this.send(CMD.BLE_UNPAIR); };

  /** Bound/connected remote info. */
  MiRC003.prototype.bleInfo = function () { return this.send(CMD.BLE_INFO); };

  /** Reconnect to the bound remote. */
  MiRC003.prototype.bleReconnect = function () { return this.send(CMD.BLE_RECONNECT); };

  /**
   * Dump the next few HID notifications and ATVV control frames verbatim.
   *
   * Diagnostic aid for a remote whose report layout is unknown: press the
   * button that does nothing, then read the log. Returns { raw_report: bool }.
   */
  MiRC003.prototype.bleRawReport = function (on) {
    return this.send(CMD.BLE_RAW_REPORT, [on === false ? 0 : 1]);
  };

  /**
   * Select the HID report dialect: "auto", "rc003" or "google_tv".
   *
   * The two supported remotes multiplex different payloads onto the same
   * characteristic and the bytes are ambiguous on their own, so "auto" decides
   * from the advertised device name and this overrides it. Returns
   * { layout: string }.
   */
  MiRC003.prototype.bleLayout = function (layout) {
    var name = layout == null ? "auto" : String(layout);
    var bytes = [];
    for (var i = 0; i < name.length; i++) bytes.push(name.charCodeAt(i) & 0xFF);
    return this.send(CMD.BLE_LAYOUT, bytes);
  };

  /**
   * Read the user-defined name for each physical slot.
   *
   * Always returns exactly {@link MiRC003.KEY_SLOT_COUNT} strings in slot order;
   * `""` means "no custom name, fall back to the built-in label". A firmware
   * build without this opcode rejects, so callers that want to stay compatible
   * with older devices must catch.
   *
   * @returns {Promise<string[]>}
   */
  MiRC003.prototype.keyNames = function () {
    return this.send(CMD.KEY_NAMES_GET).then(function (res) {
      return normalizeNames(res && res.names);
    });
  };

  /**
   * Rename one physical slot and persist it.
   *
   * `name` is truncated to {@link MiRC003.KEY_NAME_MAX_BYTES} UTF-8 bytes on a
   * character boundary before it is sent; pass `""` to clear the custom name and
   * go back to the built-in label.
   *
   * @param {number} slot Physical slot, `0` .. `KEY_SLOT_COUNT - 1`.
   * @param {string} name New name (empty string restores the default).
   * @returns {Promise<string[]>} the full name table after the write.
   */
  MiRC003.prototype.setKeyName = function (slot, name) {
    // Validate the raw argument rather than a coerced number: Number(null) and
    // Number("") are both 0, so coercing first would silently redirect a
    // malformed call at slot 0 - a real write to the wrong button.
    if (!Number.isInteger(slot) || slot < 0 || slot >= KEY_SLOT_COUNT) {
      return Promise.reject(new Error("按键槽位超出范围（0-" + (KEY_SLOT_COUNT - 1) + "）"));
    }
    var text = truncateUtf8(name, KEY_NAME_MAX_BYTES);
    return this.send(CMD.KEY_NAMES_SET, { slot: slot, name: text }).then(function (res) {
      return normalizeNames(res && res.names);
    });
  };

  /** Recent device logs: { logs: string[] }. */
  MiRC003.prototype.getLogs = function () { return this.send(CMD.LOGS_GET); };

  /** Clear the device log buffer. */
  MiRC003.prototype.clearLogs = function () { return this.send(CMD.LOGS_CLEAR); };

  /** Reboot the device. */
  MiRC003.prototype.restart = function () { return this.send(CMD.SYSTEM_RESTART); };

  /** Erase NVS (factory reset) and reboot. */
  MiRC003.prototype.factoryReset = function () { return this.send(CMD.NVS_RESET); };

  /* ------------------------------- statics ------------------------------- */

  MiRC003.CMD = CMD;
  MiRC003.STATUS = STATUS;
  MiRC003.ACTIONS = ACTIONS;
  MiRC003.ACTION = ACTION;
  MiRC003.GESTURES = GESTURES;
  MiRC003.PHYSICAL_KEYS = PHYSICAL_KEYS;
  MiRC003.KEY_SLOT_COUNT = KEY_SLOT_COUNT;
  MiRC003.DEFAULT_KEY_NAMES = DEFAULT_KEY_NAMES;
  MiRC003.KEY_NAME_MAX_BYTES = KEY_NAME_MAX_BYTES;
  /** See {@link truncateUtf8} - exposed so a UI can validate input as it is typed. */
  MiRC003.truncateKeyName = function (str) { return truncateUtf8(str, KEY_NAME_MAX_BYTES); };
  MiRC003.SWITCH_MAP_DEFAULT = SWITCH_MAP_DEFAULT;
  MiRC003.SWITCH_MAP_LOCKED = SWITCH_MAP_LOCKED;
  MiRC003.MOD_BITS = MOD_BITS;
  MiRC003.MOUSE_BUTTONS = MOUSE_BUTTONS;
  MiRC003.HID_GROUPS = HID_GROUPS;
  MiRC003.HID_EXTRA_GROUPS = HID_EXTRA_GROUPS;
  MiRC003.HID_MODIFIERS = HID_MODIFIERS;
  MiRC003.CONSUMER_GROUPS = CONSUMER_GROUPS;
  MiRC003.Keymap = Keymap;

  global.MiRC003 = MiRC003;
})(window);
