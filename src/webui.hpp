// ==============================================================================
// LAIS (Local AI Studio) v3.7.0 - Embedded SPA WebUI
// Features:
//   - Dynamic Content Filter Toggle: Safe Mode (검열 적용, Standard Models) <->
//     Uncensored Mode (검열 해제, Abliterated Models) with Cookie Persistence
//   - Dark Mode / White (Light) Mode with Browser Cookie Persistence
//   - Router-Style Single-Admin PAKE Login, Mandatory Initial Password Change,
//     Auto-Login Cookie, and Account/Theme Settings Modal
//   - Collapsible Muted Thinking Process (<details> hidden by default) +
//     Prominent Final Output (Zero raw <think> or HTML tags)
//   - Conversation Branching (Branch Here / Edit & Branch) + Copy Prompt/Result
//   - Drag-and-Drop File Attachment & Direct Output Download Links
// SPDX-License-Identifier: MIT
// ==============================================================================
#pragma once

namespace lais {

inline constexpr const char* WEBUI_HTML = R"HTML(<!DOCTYPE html>
<html lang="en" data-theme="dark">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>LAIS - Local AI Studio (C++20 Multimodal Router)</title>
<style>
  :root, html[data-theme="dark"] {
    --bg: #0b0f19;
    --panel: #111827;
    --panel-alt: #0f172a;
    --sidebar: #0d1322;
    --border: #1f2937;
    --input-bg: #0b0f19;
    --input-border: #374151;
    --text: #f3f4f6;
    --text-strong: #ffffff;
    --muted: #9ca3af;
    --faint: #6b7280;
    --accent: #3b82f6;
    --user-msg-bg: #1e3a8a;
    --user-msg-border: #2563eb;
    --user-msg-text: #f8fafc;
    --think-bg: rgba(15, 23, 42, 0.55);
    --think-border: rgba(148, 163, 184, 0.22);
    --think-text: #94a3b8;
    --high: #8b5cf6;
    --eco: #10b981;
    --warn: #f59e0b;
    --danger: #ef4444;
    --shadow: rgba(0, 0, 0, 0.5);
  }
  html[data-theme="light"] {
    --bg: #f8fafc;
    --panel: #ffffff;
    --panel-alt: #f1f5f9;
    --sidebar: #f1f5f9;
    --border: #e2e8f0;
    --input-bg: #ffffff;
    --input-border: #cbd5e1;
    --text: #1e293b;
    --text-strong: #0f172a;
    --muted: #475569;
    --faint: #64748b;
    --accent: #2563eb;
    --user-msg-bg: #dbeafe;
    --user-msg-border: #93c5fd;
    --user-msg-text: #1e3a8a;
    --think-bg: #f8fafc;
    --think-border: #cbd5e1;
    --think-text: #64748b;
    --high: #7c3aed;
    --eco: #059669;
    --warn: #d97706;
    --danger: #dc2626;
    --shadow: rgba(15, 23, 42, 0.12);
  }
  * { box-sizing: border-box; }
  body {
    margin: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
    background: var(--bg); color: var(--text); display: flex; flex-direction: column; height: 100vh; overflow: hidden;
    transition: background 0.18s ease, color 0.18s ease;
  }
  header {
    background: var(--panel); border-bottom: 1px solid var(--border);
    padding: 10px 18px; display: flex; flex-wrap: wrap; align-items: center; justify-content: space-between; gap: 12px;
  }
  .brand { display: flex; align-items: center; gap: 10px; font-weight: 700; font-size: 1.02rem; color: var(--text-strong); }
  .badge {
    font-size: 0.74rem; padding: 3px 9px; border-radius: 999px; font-weight: 600;
    background: rgba(59, 130, 246, 0.16); color: var(--accent); border: 1px solid rgba(59, 130, 246, 0.35);
  }
  .meters { display: flex; align-items: center; gap: 12px; flex-wrap: wrap; font-size: 0.8rem; }
  .meter-box { display: flex; flex-direction: column; gap: 3px; min-width: 165px; }
  .bar-bg { width: 100%; height: 6px; background: var(--border); border-radius: 4px; overflow: hidden; }
  .bar-fill { height: 100%; background: var(--eco); transition: width 0.25s ease; }
  .controls {
    background: var(--panel-alt); border-bottom: 1px solid var(--border);
    padding: 9px 18px; display: flex; flex-wrap: wrap; gap: 12px; align-items: center; font-size: 0.83rem;
  }
  select, button, input[type="text"], input[type="password"], input[type="number"] {
    background: var(--input-bg); color: var(--text); border: 1px solid var(--input-border);
    border-radius: 6px; padding: 6px 10px; font-size: 0.83rem;
  }
  button { cursor: pointer; transition: opacity 0.15s ease, transform 0.1s ease; }
  button:hover { opacity: 0.92; }
  button:disabled { cursor: wait; opacity: 0.65; }
  .generation-active .chat-item { pointer-events: none; opacity: 0.65; }
  button.primary { background: var(--accent); border-color: var(--accent); color: #fff; font-weight: 600; }
  button.secondary { background: var(--panel-alt); border-color: var(--input-border); color: var(--text); font-weight: 500; }
  button.danger { background: #7f1d1d; border-color: #991b1b; color: #fecaca; }
  label.toggle { display: inline-flex; align-items: center; gap: 6px; cursor: pointer; user-select: none; }
  /* Safety / Uncensored Pill Toggle */
  .safety-pill {
    display: inline-flex; align-items: center; gap: 7px; padding: 4px 11px; border-radius: 999px;
    font-size: 0.8rem; font-weight: 700; cursor: pointer; user-select: none;
    transition: all 0.15s ease;
  }
  .safety-pill.safe {
    background: rgba(16, 185, 129, 0.16); color: var(--eco); border: 1px solid rgba(16, 185, 129, 0.45);
  }
  .safety-pill.uncensored {
    background: rgba(245, 158, 11, 0.18); color: var(--warn); border: 1px solid rgba(245, 158, 11, 0.5);
  }
  .workspace { flex: 1; display: flex; min-height: 0; position: relative; }
  aside.sidebar {
    width: 265px; background: var(--sidebar); border-right: 1px solid var(--border);
    display: flex; flex-direction: column; min-height: 0;
  }
  .sidebar-header {
    padding: 12px; border-bottom: 1px solid var(--border); display: flex; justify-content: space-between; align-items: center; gap: 8px;
  }
  .chat-list { flex: 1; overflow-y: auto; padding: 8px; display: flex; flex-direction: column; gap: 4px; }
  .chat-item {
    display: flex; align-items: center; justify-content: space-between; gap: 6px;
    padding: 8px 10px; border-radius: 7px; font-size: 0.83rem; cursor: pointer;
    color: var(--text); border: 1px solid transparent;
  }
  .chat-item:hover { background: rgba(59, 130, 246, 0.08); }
  .chat-item.active { background: rgba(59, 130, 246, 0.16); border-color: rgba(59, 130, 246, 0.45); font-weight: 600; }
  .chat-title { flex: 1; overflow: hidden; text-overflow: ellipsis; white-space: nowrap; }
  .chat-del {
    background: transparent; border: none; color: var(--muted); font-size: 0.85rem; padding: 2px 5px; border-radius: 4px;
  }
  .chat-del:hover { color: var(--danger); background: rgba(239, 68, 68, 0.15); }
  .chat-pane { flex: 1; display: flex; flex-direction: column; min-width: 0; min-height: 0; position: relative; }
  main { flex: 1; overflow-y: auto; padding: 18px 22px; display: flex; flex-direction: column; gap: 14px; }
  .msg { max-width: 900px; padding: 14px 16px; border-radius: 10px; line-height: 1.6; word-break: break-word; }
  .msg.user {
    align-self: flex-end; background: var(--user-msg-bg); border: 1px solid var(--user-msg-border);
    color: var(--user-msg-text); min-width: 240px;
  }
  .msg.assistant { align-self: flex-start; background: var(--panel); border: 1px solid var(--border); width: 100%; }
  .route-tag {
    display: inline-block; font-size: 0.72rem; padding: 2px 8px; border-radius: 4px;
    margin-bottom: 8px; background: rgba(139, 92, 246, 0.16); color: var(--high); border: 1px solid rgba(139, 92, 246, 0.35);
    font-weight: 600;
  }
  .attach-badge {
    display: inline-flex; align-items: center; gap: 6px; font-size: 0.76rem;
    padding: 3px 9px; border-radius: 6px; background: rgba(16, 185, 129, 0.16);
    border: 1px solid rgba(16, 185, 129, 0.4); color: var(--eco); margin-bottom: 6px;
  }
  /* Collapsible, faint Thinking Process block (Hidden by default) */
  details.think-box {
    margin: 4px 0 12px 0; padding: 7px 11px; border-radius: 7px;
    background: var(--think-bg); border: 1px dashed var(--think-border);
    color: var(--think-text); font-size: 0.82rem; opacity: 0.82;
  }
  details.think-box summary {
    cursor: pointer; user-select: none; font-weight: 500; color: var(--faint);
    outline: none; list-style: none; display: flex; align-items: center; gap: 6px;
  }
  details.think-box summary::-webkit-details-marker { display: none; }
  details.think-box summary::before {
    content: "▸"; display: inline-block; transition: transform 0.15s ease; font-size: 0.85rem;
  }
  details.think-box[open] summary::before { transform: rotate(90deg); }
  .think-content {
    margin-top: 8px; padding-top: 8px; border-top: 1px solid var(--think-border);
    white-space: pre-wrap; color: var(--think-text); font-size: 0.81rem; line-height: 1.5;
  }
  /* Prominent, high-clarity Final Output */
  .final-output {
    white-space: pre-wrap; color: var(--text-strong); font-size: 0.96rem; line-height: 1.62; font-weight: 450;
  }
  .msg img, .msg video { max-width: 100%; border-radius: 8px; margin-top: 10px; border: 1px solid var(--border); display: block; }
  .action-bar {
    margin-top: 11px; padding-top: 8px; border-top: 1px solid rgba(148, 163, 184, 0.15);
    display: flex; flex-wrap: wrap; gap: 8px; align-items: center;
  }
  .act-btn, .dl-link {
    display: inline-flex; align-items: center; gap: 5px; padding: 4px 10px;
    border-radius: 6px; font-size: 0.76rem; font-weight: 600; text-decoration: none;
    background: var(--panel-alt); color: var(--muted); border: 1px solid var(--input-border);
    cursor: pointer;
  }
  .act-btn:hover { color: var(--text-strong); border-color: var(--accent); }
  .dl-link {
    background: rgba(16, 185, 129, 0.15); color: var(--eco); border-color: rgba(16, 185, 129, 0.4);
  }
  .dl-link:hover { background: rgba(16, 185, 129, 0.25); }
  footer {
    background: var(--panel); border-top: 1px solid var(--border);
    padding: 10px 18px; display: flex; flex-direction: column; gap: 8px;
  }
  .attachment-strip { display: none; align-items: center; gap: 8px; flex-wrap: wrap; }
  .attachment-chip {
    display: inline-flex; align-items: center; gap: 8px; padding: 4px 10px;
    border-radius: 999px; font-size: 0.78rem; background: var(--panel-alt); border: 1px solid var(--accent); color: var(--accent);
  }
  .attachment-chip button {
    background: transparent; border: none; color: var(--danger); font-weight: 700; padding: 0 4px; cursor: pointer;
  }
  .composer-row { display: flex; gap: 10px; align-items: flex-end; }
  textarea {
    flex: 1; background: var(--input-bg); color: var(--text-strong); border: 1px solid var(--input-border);
    border-radius: 8px; padding: 10px 12px; font-size: 0.94rem; resize: vertical; min-height: 52px; max-height: 180px;
  }
  .drop-overlay {
    display: none; position: fixed; inset: 0; z-index: 900;
    background: rgba(15, 23, 42, 0.84); border: 3px dashed var(--accent);
    align-items: center; justify-content: center; flex-direction: column; gap: 10px;
    color: #93c5fd; font-size: 1.25rem; font-weight: 700; pointer-events: none;
  }
  .drop-overlay.active { display: flex; }
  .modal-backdrop {
    display: none; position: fixed; inset: 0; z-index: 1000;
    background: rgba(11, 15, 25, 0.82); backdrop-filter: blur(4px);
    align-items: center; justify-content: center; padding: 16px;
  }
  .modal-backdrop.active { display: flex; }
  .modal-card {
    background: var(--panel); border: 1px solid var(--border); border-radius: 12px;
    width: 100%; max-width: 450px; padding: 22px 24px; box-shadow: 0 18px 40px var(--shadow);
    display: flex; flex-direction: column; gap: 13px;
  }
  .modal-card h2 { margin: 0; font-size: 1.12rem; color: var(--text-strong); }
  .modal-sub { font-size: 0.82rem; color: var(--muted); line-height: 1.45; }
  .form-field { display: flex; flex-direction: column; gap: 5px; font-size: 0.82rem; }
  .form-field label { font-weight: 600; color: var(--text); }
  .form-field input { width: 100%; padding: 8px 10px; font-size: 0.9rem; }
  .notice-box {
    padding: 9px 12px; border-radius: 7px; font-size: 0.8rem; line-height: 1.45;
    background: rgba(245, 158, 11, 0.14); border: 1px solid rgba(245, 158, 11, 0.4); color: var(--warn);
  }
  .err-box {
    display: none; padding: 8px 11px; border-radius: 6px; font-size: 0.8rem;
    background: rgba(239, 68, 68, 0.15); border: 1px solid rgba(239, 68, 68, 0.4); color: var(--danger);
  }
  .ok-box {
    display: none; padding: 8px 11px; border-radius: 6px; font-size: 0.8rem;
    background: rgba(16, 185, 129, 0.15); border: 1px solid rgba(16, 185, 129, 0.4); color: var(--eco);
  }
</style>
</head>
<body>
<div class="drop-overlay" id="dropOverlay">
  <div><span data-i18n="drop.title">Drop a file here to attach it</span></div>
  <div style="font-size:0.9rem;color:#cbd5e1;font-weight:400;"><span data-i18n="drop.help">Attach images for visual analysis or text/code files as prompt context.</span></div>
</div>

<!-- Router-Style PAKE Login Modal -->
<div class="modal-backdrop" id="loginModal">
  <div class="modal-card">
    <div style="align-self:flex-end;"><select class="ui-language-select" aria-label="Interface language" data-i18n-aria-label="language.label" onchange="changeUiLanguage(this.value)"><option value="en-us">English</option><option value="ko-kr">한국어</option></select></div>
    <h2><span data-i18n="auth.title">🔒 Sign in to LAIS</span></h2>
    <div class="modal-sub"><span data-i18n="auth.description">Sign in with your administrator account. Your password is not sent in plaintext or stored in the browser.</span></div>
    <div class="notice-box" id="defaultCredNotice" style="display:none;" data-i18n="auth.default_notice">Initial ID: admin / password: admin. Set new credentials after signing in.</div>
    <div class="err-box" id="loginErr"></div>
    <div class="form-field">
      <label for="loginUser"><span data-i18n="auth.user">Administrator ID</span></label>
      <input type="text" id="loginUser" value="admin" autocomplete="username">
    </div>
    <div class="form-field">
      <label for="loginPass"><span data-i18n="auth.password">Password</span></label>
      <input type="password" id="loginPass" placeholder="Enter your password" data-i18n-placeholder="auth.password_placeholder" autocomplete="current-password">
    </div>
    <label class="toggle" style="font-size:0.83rem;">
      <input type="checkbox" id="loginAuto" checked>
      <span><span data-i18n="auth.remember">Keep me signed in on this browser (30 days)</span></span>
    </label>
    <button class="primary" onclick="performPakeLogin()" style="padding:9px 14px;font-size:0.9rem;"><span data-i18n="auth.sign_in">Sign in</span></button>
  </div>
</div>

<!-- Browser Settings & Admin Credential Management Modal -->
<div class="modal-backdrop" id="settingsModal">
  <div class="modal-card" style="max-width:470px;">
    <div style="display:flex;justify-content:space-between;align-items:center;">
      <h2 id="settingsModalTitle"><span data-i18n="settings.title"><span data-i18n="settings.open">⚙️ Settings</span></span></h2>
      <button class="secondary" id="settingsCloseBtn" onclick="closeSettingsModal()" style="padding:3px 9px;">✕</button>
    </div>
    <div class="notice-box" id="mustChangeBanner" style="display:none;" data-i18n="settings.change_required">You signed in with the initial account. Set your new administrator ID and password.</div>
    <div class="err-box" id="settingsErr"></div>
    <div class="ok-box" id="settingsOk"></div>

    <div class="form-field">
      <label><span data-i18n="settings.theme">Appearance</span></label>
      <div style="display:flex;gap:8px;">
        <button type="button" class="secondary" style="flex:1;" onclick="setTheme('dark')"><span data-i18n="theme.dark_mode">🌙 Dark mode</span></button>
        <button type="button" class="secondary" style="flex:1;" onclick="setTheme('light')"><span data-i18n="theme.light_mode">☀️ Light mode</span></button>
      </div>
    </div>

    <div class="form-field">
      <label><span data-i18n="settings.models">Model mode</span></label>
      <div style="display:flex;gap:8px;">
        <button type="button" class="secondary" style="flex:1;" onclick="setUncensoredMode(false)"><span data-i18n="mode.standard">🛡️ Standard</span></button>
        <button type="button" class="secondary" style="flex:1;" onclick="setUncensoredMode(true)"><span data-i18n="mode.uncensored">🔓 Uncensored</span></button>
      </div>
    </div>

    <hr style="border:none;border-top:1px solid var(--border);margin:2px 0;">
    <div style="font-weight:700;font-size:0.88rem;color:var(--text-strong);"><span data-i18n="settings.account">Change administrator credentials</span></div>

    <div class="form-field">
      <label for="curAdminPass"><span data-i18n="settings.current_password">Current password</span></label>
      <input type="password" id="curAdminPass" placeholder="Current password (initial default: admin)" data-i18n-placeholder="settings.current_placeholder">
    </div>
    <div class="form-field">
      <label for="newAdminUser"><span data-i18n="settings.new_user">New administrator ID</span></label>
      <input type="text" id="newAdminUser" placeholder="New administrator ID" data-i18n-placeholder="settings.user_placeholder">
    </div>
    <div class="form-field">
      <label for="newAdminPass"><span data-i18n="settings.new_password">New password</span></label>
      <input type="password" id="newAdminPass" placeholder="Enter a new password" data-i18n-placeholder="settings.new_password_placeholder">
    </div>
    <div class="form-field">
      <label for="newAdminPassConfirm"><span data-i18n="settings.confirm_password">Confirm new password</span></label>
      <input type="password" id="newAdminPassConfirm" placeholder="Enter the new password again" data-i18n-placeholder="settings.confirm_placeholder">
    </div>

    <div style="display:flex;gap:8px;justify-content:space-between;margin-top:4px;">
      <button type="button" class="danger" onclick="performLogout()"><span data-i18n="auth.sign_out">Sign out</span></button>
      <button type="button" class="primary" onclick="updateAdminCredentials()" style="flex:1;"><span data-i18n="settings.save">Save credentials</span></button>
    </div>
  </div>
</div>

<header>
  <div class="brand">
    <span>LAIS v3.7 • Local AI Studio</span>
    <span class="badge" id="activeModelBadge"><span data-i18n="status.idle">Idle</span></span>
  </div>
  <div class="meters">
    <select class="ui-language-select" aria-label="Interface language" data-i18n-aria-label="language.label" onchange="changeUiLanguage(this.value)"><option value="en-us">English</option><option value="ko-kr">한국어</option></select>
    <div class="meter-box">
      <div style="display:flex;justify-content:space-between;"><span><span data-i18n="status.vram">GPU memory (16 GB)</span></span><span id="vramText">-- / -- MB</span></div>
      <div class="bar-bg"><div class="bar-fill" id="vramBar" style="width:10%"></div></div>
    </div>
    <div class="meter-box">
      <div style="display:flex;justify-content:space-between;"><span><span data-i18n="status.dram">System memory (32 GB)</span></span><span id="dramText">-- / -- MB</span></div>
      <div class="bar-bg"><div class="bar-fill" id="dramBar" style="width:10%"></div></div>
    </div>
    <button class="secondary" id="themeBtn" onclick="toggleTheme()" title="Switch light / dark appearance" data-i18n-title="theme.toggle"><span data-i18n="theme.light">☀️ Light</span></button>
    <button class="secondary" onclick="openSettingsModal(false)" title="Open settings" data-i18n-title="settings.tooltip"><span data-i18n="settings.open">⚙️ Settings</span></button>
    <button class="danger" onclick="purgeMemory()"><span data-i18n="memory.release">Release memory</span></button>
  </div>
</header>

<div class="controls">
  <label><span data-i18n="task.label">Task:</span>
    <select id="taskMode">
      <option value="chat" data-i18n="task.chat">Chat / reasoning</option>
      <option value="coder" data-i18n="task.coder">Coding</option>
      <option value="image" data-i18n="task.image">Image generation</option>
      <option value="video" data-i18n="task.video">Video generation</option>
      <option value="music" data-i18n="task.music">Music / vocals</option>
    </select>
  </label>

  <label class="safety-pill safe" id="safetyPillLabel" title="Select separate standard or uncensored model pipelines." data-i18n-title="mode.tooltip">
    <input type="checkbox" id="uncensoredToggle" onchange="onUncensoredCheckboxChange()" style="margin:0;">
    <span id="safetyPillText"><span data-i18n="mode.standard">🛡️ Standard</span></span>
  </label>

  <label class="toggle" title="Enable the model’s reasoning mode." data-i18n-title="reasoning.tooltip">
    <input type="checkbox" id="reasoningToggle">
    <strong><span data-i18n="reasoning.label">Reasoning</span></strong>
  </label>

  <label><span data-i18n="tier.label">Quality:</span>
    <select id="tierSelect">
      <option value="auto" data-i18n="tier.auto">Auto (quality within available VRAM)</option>
      <option value="high" data-i18n="tier.high">High (Eco fallback if needed)</option>
      <option value="eco" data-i18n="tier.eco">Eco (lower VRAM use)</option>
    </select>
  </label>

  <label class="toggle">
    <input type="checkbox" id="webSearchToggle">
    <span><span data-i18n="search.label">Web search</span></span>
  </label>

  <label><span data-i18n="attachment.label">Attachment:</span>
    <input type="file" id="fileInput" style="display:none;"><button type="button" class="secondary" onclick="document.getElementById('fileInput').click()" data-i18n="attachment.choose">Choose file</button>
  </label>
</div>

<div class="workspace">
  <aside class="sidebar">
    <div class="sidebar-header">
      <strong style="font-size:0.85rem;"><span data-i18n="chats.title">Saved conversations</span></strong>
      <button class="primary" id="newChatButton" onclick="startNewChat()" style="padding:4px 9px;font-size:0.78rem;"><span data-i18n="chats.new">+ New conversation</span></button>
    </div>
    <div class="chat-list" id="chatList"></div>
  </aside>

  <div class="chat-pane">
    <main id="chatStream"></main>

    <footer>
      <div class="attachment-strip" id="attachmentStrip">
        <span class="attachment-chip">
          <span id="attachmentLabel">Attached: file</span>
          <button type="button" onclick="clearAttachment()" title="Remove attachment" data-i18n-title="attachment.remove">&times;</button>
        </span>
      </div>
      <div class="composer-row">
        <textarea id="promptInput" placeholder="Write in any language or drop a file. Enter sends; Shift+Enter adds a line." data-i18n-placeholder="prompt.placeholder"></textarea>
        <button id="sendButton" type="button" class="primary" onclick="handleSendButton()" style="height:52px;padding:0 22px;" aria-label="Send" data-i18n-aria-label="prompt.send"><span id="sendButtonLabel" data-i18n="prompt.send">Send</span></button>
      </div>
    </footer>
  </div>
</div>

<script>// Language bundles affect interface strings only; never mutate model prompts or messages.
const FALLBACK_UI_EN = {"drop.title":"Drop a file here to attach it","drop.help":"Attach images for visual analysis or text/code files as prompt context.","auth.title":"🔒 Sign in to LAIS","auth.description":"Sign in with your administrator account. Your password is not sent in plaintext or stored in the browser.","auth.user":"Administrator ID","auth.password":"Password","auth.remember":"Keep me signed in on this browser (30 days)","auth.sign_in":"Sign in","settings.title":"⚙️ Settings","settings.theme":"Appearance","theme.dark_mode":"🌙 Dark mode","theme.light_mode":"☀️ Light mode","settings.models":"Model mode","mode.standard":"🛡️ Standard","mode.uncensored":"🔓 Uncensored","settings.account":"Change administrator credentials","settings.current_password":"Current password","settings.new_user":"New administrator ID","settings.new_password":"New password","settings.confirm_password":"Confirm new password","auth.sign_out":"Sign out","settings.save":"Save credentials","status.idle":"Idle","status.vram":"GPU memory (16 GB)","status.dram":"System memory (32 GB)","theme.light":"☀️ Light","settings.open":"⚙️ Settings","memory.release":"Release memory","task.label":"Task:","task.chat":"Chat / reasoning","task.coder":"Coding","task.image":"Image generation","task.video":"Video generation","task.music":"Music / vocals","reasoning.label":"Reasoning","tier.label":"Quality:","tier.auto":"Auto (quality within available VRAM)","tier.high":"High (Eco fallback if needed)","tier.eco":"Eco (lower VRAM use)","search.label":"Web search","attachment.label":"Attachment:","chats.title":"Saved conversations","chats.new":"+ New conversation","prompt.send":"Send","auth.default_notice":"Initial ID: admin / password: admin. Set new credentials after signing in.","settings.change_required":"You signed in with the initial account. Set your new administrator ID and password.","auth.password_placeholder":"Enter your password","settings.current_placeholder":"Current password (initial default: admin)","settings.user_placeholder":"New administrator ID","settings.new_password_placeholder":"Enter a new password","settings.confirm_placeholder":"Enter the new password again","prompt.placeholder":"Write in any language or drop a file. Enter sends; Shift+Enter adds a line.","theme.toggle":"Switch light / dark appearance","settings.tooltip":"Open settings","mode.tooltip":"Select separate standard or uncensored model pipelines.","reasoning.tooltip":"Enable the model’s reasoning mode.","attachment.remove":"Remove attachment","language.label":"Interface language","attachment.choose":"Choose file","app.title":"LAIS · Local AI Studio","theme.dark":"🌙 Dark","auth.enter_credentials":"Enter both administrator ID and password.","auth.init_failed":"Could not initialize sign-in.","auth.invalid":"Invalid credentials.","auth.error":"Sign-in failed: {error}","settings.fill_fields":"Enter the current password, new administrator ID and new password.","settings.password_mismatch":"The new passwords do not match.","settings.change_default":"Choose a password different from the initial default.","settings.challenge_failed":"Could not verify the current account.","settings.update_failed":"Could not update the credentials.","settings.updated":"Administrator credentials saved.","settings.error":"Could not save: {error}","welcome.title":"LAIS · Separate model modes · Multilingual","welcome.models":"Choose a standard or uncensored pipeline. Each mode keeps its own model assignments; it does not fall back to the other mode.","welcome.language":"Interface language changes only the interface. You can continue to write to the models in any supported language.","welcome.tools":"Attach images or files, inspect reasoning, copy results or branch a conversation.","copy.done":"✓ Copied","copy.prompt":"📋 Copy prompt","copy.result":"📋 Copy result","attachment.image":"Image: {name}","attachment.file":"File: {name}","attachment.badge":"Attached: {name}","chats.empty":"No saved conversations yet.","chats.count":"{title} ({count} messages)","chats.delete":"Delete conversation","thinking.show":"💭 Show reasoning","branch.here":"🌿 Branch here","branch.tooltip":"Create a new conversation from this message","branch.edit":"✏️ Edit and branch","branch.edit_tooltip":"Start a branch before this prompt and edit it","download.file":"⬇ Download ({name})","prompt.analyze_file":"Please analyze the attached file.","prompt.analyze_image":"Describe and analyze this image.","prompt.attached":"[Attached: {name}]","request.pending":"Selecting a model and generating a response…","request.empty":"No response returned.","request.failed":"Request failed: {error}","status.routing":"Preparing","status.error":"Error","prompt.stop":"■ Stop generating","prompt.stopping":"Stopping…","prompt.stop_tooltip":"Stop the current generation","request.stopping":"Stopping generation…","request.stopped":"Generation stopped.","request.stop_failed":"Could not stop generation: {error}. Please try again."};
const GENERATION_UI_KO = {"prompt.stop":"■ 생성 중지","prompt.stopping":"중지 중…","prompt.stop_tooltip":"현재 생성 작업 중지","request.stopping":"생성 작업을 중지하고 있습니다…","request.stopped":"생성을 중지했습니다.","request.stop_failed":"생성 중지 실패: {error}. 다시 시도해 주세요."};
let uiStrings = {...FALLBACK_UI_EN};
let currentUiLanguage = 'en-us';
let languageRequestId = 0;
function tUi(key, values = {}) {
  const template = uiStrings[key] || FALLBACK_UI_EN[key] || key;
  return template.replace(/\{([a-zA-Z_][a-zA-Z0-9_]*)\}/g, (whole, name) =>
    Object.prototype.hasOwnProperty.call(values, name) ? String(values[name]) : whole);
}
function applyUiLanguage() {
  document.documentElement.lang = currentUiLanguage === 'ko-kr' ? 'ko-KR' : 'en-US';
  document.title = tUi('app.title');
  document.querySelectorAll('[data-i18n]').forEach(el => {
    let values = {};
    try { values = JSON.parse(el.dataset.i18nVars || '{}'); } catch (_) {}
    el.textContent = tUi(el.dataset.i18n, values);
  });
  for (const attr of ['placeholder', 'title', 'aria-label']) {
    document.querySelectorAll('[data-i18n-' + attr + ']').forEach(el =>
      el.setAttribute(attr, tUi(el.getAttribute('data-i18n-' + attr))));
  }
  document.querySelectorAll('.ui-language-select').forEach(el => { el.value = currentUiLanguage; });
  setTheme(document.documentElement.getAttribute('data-theme') || 'dark');
  setUncensoredMode(document.getElementById('uncensoredToggle').checked);
  updateAttachmentUI();
}
async function changeUiLanguage(code) {
  code = code === 'ko-kr' ? 'ko-kr' : 'en-us';
  const requestId = ++languageRequestId;
  let bundle;
  try {
    const response = await fetch('/api/locales/' + code, {cache:'no-store'});
    if (!response.ok) throw new Error('Language bundle unavailable');
    bundle = await response.json();
    if (!bundle || typeof bundle !== 'object' || Array.isArray(bundle)) throw new Error('Invalid language bundle');
  } catch (error) {
    console.warn(error);
    code = 'en-us'; bundle = FALLBACK_UI_EN;
  }
  if (requestId !== languageRequestId) return;
  uiStrings = {...FALLBACK_UI_EN, ...(code === 'ko-kr' ? GENERATION_UI_KO : {})};
  for (const [key, value] of Object.entries(bundle)) if (typeof value === 'string') uiStrings[key] = value;
  currentUiLanguage = code;
  try { localStorage.setItem('lais_ui_language', code); } catch (_) {}
  setCookie('lais_ui_language', code, 365);
  applyUiLanguage();
}
async function initializeUiLanguage() {
  let saved;
  try { saved = localStorage.getItem('lais_ui_language'); } catch (_) {}
  saved = saved || getCookie('lais_ui_language');
  const selected = saved || ((navigator.language || '').toLowerCase().startsWith('ko') ? 'ko-kr' : 'en-us');
  await changeUiLanguage(selected);
}

// ==============================================================================
// 1. Cookie, Theme (Dark/White) & Content Filter (Safe/Uncensored) Management
// ==============================================================================
function setCookie(name, value, days) {
  let expires = "";
  if (days) {
    const d = new Date();
    d.setTime(d.getTime() + (days * 24 * 60 * 60 * 1000));
    expires = "; expires=" + d.toUTCString();
  }
  document.cookie = name + "=" + encodeURIComponent(value || "") + expires + "; path=/; SameSite=Lax";
}

function getCookie(name) {
  const eq = name + "=";
  const parts = document.cookie.split(';');
  for (let p of parts) {
    p = p.trim();
    if (p.indexOf(eq) === 0) return decodeURIComponent(p.substring(eq.length));
  }
  return "";
}

function setTheme(theme) {
  const t = (theme === 'light') ? 'light' : 'dark';
  document.documentElement.setAttribute('data-theme', t);
  setCookie('lais_theme', t, 365);
  const btn = document.getElementById('themeBtn');
  if (btn) btn.textContent = (t === 'dark') ? tUi('theme.light') : tUi('theme.dark');
}

function toggleTheme() {
  const cur = document.documentElement.getAttribute('data-theme') || 'dark';
  setTheme(cur === 'dark' ? 'light' : 'dark');
}

function setUncensoredMode(isUncensored) {
  const cb = document.getElementById('uncensoredToggle');
  const pill = document.getElementById('safetyPillLabel');
  const txt = document.getElementById('safetyPillText');
  const unc = !!isUncensored;
  if (cb) cb.checked = unc;
  if (pill) {
    pill.className = unc ? 'safety-pill uncensored' : 'safety-pill safe';
  }
  if (txt) {
    txt.textContent = unc ? tUi('mode.uncensored') : tUi('mode.standard');
  }
  setCookie('lais_uncensored', unc ? '1' : '0', 365);
}

function onUncensoredCheckboxChange() {
  const cb = document.getElementById('uncensoredToggle');
  setUncensoredMode(cb && cb.checked);
}

(function initBrowserPreferencesFromCookies() {
  const savedTheme = getCookie('lais_theme');
  setTheme(savedTheme === 'light' ? 'light' : 'dark');
  const savedUnc = getCookie('lais_uncensored');
  setUncensoredMode(savedUnc === '1');
})();

// ==============================================================================
// 2. Pure-JS SHA-256 / HMAC-SHA256 / PBKDF2-HMAC-SHA256 for LAN + Localhost PAKE
// ==============================================================================
const SHA256_K = new Uint32Array([
  0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
  0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
  0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
  0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
  0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
  0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
  0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
  0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
]);

function utf8ToBytes(str) {
  return new TextEncoder().encode(str);
}

function bytesToHex(bytes) {
  let s = "";
  for (let i = 0; i < bytes.length; i++) {
    s += bytes[i].toString(16).padStart(2, '0');
  }
  return s;
}

function randomHex(numBytes) {
  const b = new Uint8Array(numBytes);
  if (window.crypto && window.crypto.getRandomValues) {
    window.crypto.getRandomValues(b);
  } else {
    for (let i = 0; i < numBytes; i++) b[i] = Math.floor(Math.random() * 256);
  }
  return bytesToHex(b);
}

function rotr32(x, n) {
  return (x >>> n) | (x << (32 - n));
}

function sha256Bytes(msgBytes) {
  const len = msgBytes.length;
  const bitLenHi = Math.floor((len * 8) / 0x100000000);
  const bitLenLo = (len * 8) >>> 0;
  const padLen = ((len + 9 + 63) & ~63);
  const buf = new Uint8Array(padLen);
  buf.set(msgBytes);
  buf[len] = 0x80;
  const view = new DataView(buf.buffer);
  view.setUint32(padLen - 8, bitLenHi, false);
  view.setUint32(padLen - 4, bitLenLo, false);

  let H0 = 0x6a09e667, H1 = 0xbb67ae85, H2 = 0x3c6ef372, H3 = 0xa54ff53a;
  let H4 = 0x510e527f, H5 = 0x9b05688c, H6 = 0x1f83d9ab, H7 = 0x5be0cd19;
  const W = new Uint32Array(64);

  for (let offset = 0; offset < padLen; offset += 64) {
    for (let i = 0; i < 16; i++) {
      W[i] = view.getUint32(offset + i * 4, false);
    }
    for (let i = 16; i < 64; i++) {
      const s0 = rotr32(W[i - 15], 7) ^ rotr32(W[i - 15], 18) ^ (W[i - 15] >>> 3);
      const s1 = rotr32(W[i - 2], 17) ^ rotr32(W[i - 2], 19) ^ (W[i - 2] >>> 10);
      W[i] = (W[i - 16] + s0 + W[i - 7] + s1) >>> 0;
    }
    let a = H0, b = H1, c = H2, d = H3, e = H4, f = H5, g = H6, h = H7;
    for (let i = 0; i < 64; i++) {
      const S1 = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
      const ch = (e & f) ^ ((~e) & g);
      const t1 = (h + S1 + ch + SHA256_K[i] + W[i]) >>> 0;
      const S0 = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
      const maj = (a & b) ^ (a & c) ^ (b & c);
      const t2 = (S0 + maj) >>> 0;
      h = g; g = f; f = e; e = (d + t1) >>> 0;
      d = c; c = b; b = a; a = (t1 + t2) >>> 0;
    }
    H0 = (H0 + a) >>> 0; H1 = (H1 + b) >>> 0; H2 = (H2 + c) >>> 0; H3 = (H3 + d) >>> 0;
    H4 = (H4 + e) >>> 0; H5 = (H5 + f) >>> 0; H6 = (H6 + g) >>> 0; H7 = (H7 + h) >>> 0;
  }

  const out = new Uint8Array(32);
  const outView = new DataView(out.buffer);
  outView.setUint32(0, H0, false); outView.setUint32(4, H1, false);
  outView.setUint32(8, H2, false); outView.setUint32(12, H3, false);
  outView.setUint32(16, H4, false); outView.setUint32(20, H5, false);
  outView.setUint32(24, H6, false); outView.setUint32(28, H7, false);
  return out;
}

function hmacSha256Bytes(keyBytes, msgBytes) {
  let k = keyBytes;
  if (k.length > 64) k = sha256Bytes(k);
  const kPad = new Uint8Array(64);
  kPad.set(k);
  const iPad = new Uint8Array(64 + msgBytes.length);
  const oPad = new Uint8Array(64 + 32);
  for (let i = 0; i < 64; i++) {
    iPad[i] = kPad[i] ^ 0x36;
    oPad[i] = kPad[i] ^ 0x5c;
  }
  iPad.set(msgBytes, 64);
  const inner = sha256Bytes(iPad);
  oPad.set(inner, 64);
  return sha256Bytes(oPad);
}

function pbkdf2HmacSha256Bytes(passwordStr, saltHexStr, iterations) {
  const passBytes = utf8ToBytes(passwordStr);
  const saltBytes = utf8ToBytes(saltHexStr);
  const saltBlock = new Uint8Array(saltBytes.length + 4);
  saltBlock.set(saltBytes);
  saltBlock[saltBytes.length + 3] = 1;

  let u = hmacSha256Bytes(passBytes, saltBlock);
  const t = new Uint8Array(u);
  for (let i = 1; i < iterations; i++) {
    u = hmacSha256Bytes(passBytes, u);
    for (let b = 0; b < 32; b++) t[b] ^= u[b];
  }
  return t;
}

function computeClientPakeProof(username, password, saltHex, iterations, clientNonce, serverNonce) {
  const saltedPass = pbkdf2HmacSha256Bytes(password, saltHex, iterations);
  const clientKey = hmacSha256Bytes(saltedPass, utf8ToBytes("LAIS-PAKE-Client-Key"));
  const storedKey = sha256Bytes(clientKey);
  const serverKey = hmacSha256Bytes(saltedPass, utf8ToBytes("LAIS-PAKE-Server-Key"));
  const authMsg = utf8ToBytes(`${username}:${saltHex}:${clientNonce}:${serverNonce}`);
  const clientSig = hmacSha256Bytes(storedKey, authMsg);
  const proof = new Uint8Array(32);
  for (let i = 0; i < 32; i++) proof[i] = clientKey[i] ^ clientSig[i];
  const expectedServerSig = bytesToHex(hmacSha256Bytes(serverKey, authMsg));
  return {
    clientProofHex: bytesToHex(proof),
    expectedServerSigHex: expectedServerSig
  };
}

function computeNewPakeVerifier(newPassword, newSaltHex, iterations) {
  const saltedPass = pbkdf2HmacSha256Bytes(newPassword, newSaltHex, iterations);
  const clientKey = hmacSha256Bytes(saltedPass, utf8ToBytes("LAIS-PAKE-Client-Key"));
  const storedKey = sha256Bytes(clientKey);
  const serverKey = hmacSha256Bytes(saltedPass, utf8ToBytes("LAIS-PAKE-Server-Key"));
  return {
    storedKeyHex: bytesToHex(storedKey),
    serverKeyHex: bytesToHex(serverKey)
  };
}

// ==============================================================================
// 3. Authentication & Settings Modal Workflow
// ==============================================================================
let currentAdminUser = "admin";
let forcedPasswordChange = false;

async function checkAuthStatus() {
  try {
    const r = await fetch('/api/auth/status');
    const d = await r.json();
    currentAdminUser = d.username || "admin";
    const notice = document.getElementById('defaultCredNotice');
    if (notice) notice.style.display = d.must_change_password ? 'block' : 'none';

    if (!d.authenticated) {
      document.getElementById('loginUser').value = currentAdminUser;
      document.getElementById('loginModal').classList.add('active');
      return false;
    }
    document.getElementById('loginModal').classList.remove('active');
    if (d.must_change_password) {
      openSettingsModal(true);
    }
    return true;
  } catch (e) {
    return false;
  }
}

async function performPakeLogin() {
  const errEl = document.getElementById('loginErr');
  errEl.style.display = 'none';
  const username = document.getElementById('loginUser').value.trim();
  const password = document.getElementById('loginPass').value;
  const autoLogin = document.getElementById('loginAuto').checked;

  if (!username || !password) {
    errEl.textContent = tUi('auth.enter_credentials');
    errEl.style.display = 'block';
    return;
  }

  try {
    const clientNonce = randomHex(24);
    const initResp = await fetch('/api/auth/pake-init', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ username, client_nonce: clientNonce })
    });
    const initData = await initResp.json();
    if (!initData.ok) {
      errEl.textContent = initData.error || tUi('auth.init_failed');
      errEl.style.display = 'block';
      return;
    }

    const pake = computeClientPakeProof(
      username,
      password,
      initData.salt_hex,
      initData.iterations,
      clientNonce,
      initData.server_nonce
    );

    const verResp = await fetch('/api/auth/pake-verify', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        challenge_id: initData.challenge_id,
        username,
        client_proof: pake.clientProofHex,
        auto_login: autoLogin
      })
    });
    const verData = await verResp.json();
    if (!verData.ok) {
      errEl.textContent = verData.error || tUi('auth.invalid');
      errEl.style.display = 'block';
      return;
    }

    currentAdminUser = verData.username || username;
    document.getElementById('loginPass').value = "";
    document.getElementById('loginModal').classList.remove('active');

    if (verData.must_change_password) {
      document.getElementById('curAdminPass').value = password;
      openSettingsModal(true);
    }
    await loadChatList();
    await refreshStats();
  } catch (e) {
    errEl.textContent = tUi('auth.error', {error: String(e)});
    errEl.style.display = 'block';
  }
}

document.getElementById('loginPass').addEventListener('keydown', function(e) {
  if (e.key === 'Enter') { e.preventDefault(); performPakeLogin(); }
});

function openSettingsModal(forceChange) {
  forcedPasswordChange = !!forceChange;
  document.getElementById('settingsErr').style.display = 'none';
  document.getElementById('settingsOk').style.display = 'none';
  document.getElementById('mustChangeBanner').style.display = forcedPasswordChange ? 'block' : 'none';
  document.getElementById('settingsCloseBtn').style.display = forcedPasswordChange ? 'none' : 'inline-block';
  document.getElementById('newAdminUser').value = currentAdminUser;
  document.getElementById('settingsModal').classList.add('active');
}

function closeSettingsModal() {
  if (forcedPasswordChange) return;
  document.getElementById('settingsModal').classList.remove('active');
}

async function updateAdminCredentials() {
  const errEl = document.getElementById('settingsErr');
  const okEl = document.getElementById('settingsOk');
  errEl.style.display = 'none';
  okEl.style.display = 'none';

  const curPass = document.getElementById('curAdminPass').value;
  const newUser = document.getElementById('newAdminUser').value.trim();
  const newPass = document.getElementById('newAdminPass').value;
  const confirmPass = document.getElementById('newAdminPassConfirm').value;

  if (!curPass || !newUser || !newPass) {
    errEl.textContent = tUi('settings.fill_fields');
    errEl.style.display = 'block';
    return;
  }
  if (newPass !== confirmPass) {
    errEl.textContent = tUi('settings.password_mismatch');
    errEl.style.display = 'block';
    return;
  }
  if (forcedPasswordChange && newUser === 'admin' && newPass === 'admin') {
    errEl.textContent = tUi('settings.change_default');
    errEl.style.display = 'block';
    return;
  }

  try {
    const clientNonce = randomHex(24);
    const initResp = await fetch('/api/auth/pake-init', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ username: currentAdminUser, client_nonce: clientNonce })
    });
    const initData = await initResp.json();
    if (!initData.ok) {
      errEl.textContent = initData.error || tUi('settings.challenge_failed');
      errEl.style.display = 'block';
      return;
    }

    const curProof = computeClientPakeProof(
      currentAdminUser,
      curPass,
      initData.salt_hex,
      initData.iterations,
      clientNonce,
      initData.server_nonce
    );

    const newSaltHex = randomHex(16);
    const newIterations = 10000;
    const newVerifier = computeNewPakeVerifier(newPass, newSaltHex, newIterations);

    const chgResp = await fetch('/api/auth/change-credentials', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        challenge_id: initData.challenge_id,
        current_client_proof: curProof.clientProofHex,
        new_username: newUser,
        new_salt_hex: newSaltHex,
        new_iterations: newIterations,
        new_stored_key_hex: newVerifier.storedKeyHex,
        new_server_key_hex: newVerifier.serverKeyHex
      })
    });
    const chgData = await chgResp.json();
    if (!chgData.ok) {
      errEl.textContent = chgData.error || tUi('settings.update_failed');
      errEl.style.display = 'block';
      return;
    }

    currentAdminUser = chgData.username || newUser;
    forcedPasswordChange = false;
    document.getElementById('mustChangeBanner').style.display = 'none';
    document.getElementById('settingsCloseBtn').style.display = 'inline-block';
    document.getElementById('curAdminPass').value = "";
    document.getElementById('newAdminPass').value = "";
    document.getElementById('newAdminPassConfirm').value = "";
    okEl.textContent = tUi('settings.updated');
    okEl.style.display = 'block';
  } catch (e) {
    errEl.textContent = tUi('settings.error', {error: String(e)});
    errEl.style.display = 'block';
  }
}

async function performLogout() {
  await fetch('/api/auth/logout', { method: 'POST' });
  document.getElementById('settingsModal').classList.remove('active');
  await checkAuthStatus();
}

// ==============================================================================
// 4. Chat Session, Branching, Copy & <think> Separation Logic
// ==============================================================================
let currentChatId = "";
let activeGeneration = null;
let attachedImageBase64 = "";
let attachedTextContent = "";
let attachedFileName = "";

function showWelcomeBanner() {
  const stream = document.getElementById('chatStream');
  stream.innerHTML = '';
  const div = document.createElement('div');
  div.id = 'welcomeBanner'; div.className = 'msg assistant';
  for (const key of ['welcome.title', 'welcome.models', 'welcome.language', 'welcome.tools']) {
    const p = document.createElement('p'); p.dataset.i18n = key; p.textContent = tUi(key); div.appendChild(p);
  }
  stream.appendChild(div);
}

// Client-side safeguard: strips any <think>...</think> or stray HTML thinking tags from text
function separateThinkingClientSide(rawText, explicitThinking) {
  let thinking = (explicitThinking || "").trim();
  let text = rawText || "";

  const extractRegex = /<think>([\s\S]*?)<\/think>/gi;
  let match;
  while ((match = extractRegex.exec(text)) !== null) {
    const chunk = (match[1] || "").trim();
    if (chunk && thinking.indexOf(chunk) === -1) {
      thinking = thinking ? (thinking + "\n\n" + chunk) : chunk;
    }
  }
  text = text.replace(/<think>[\s\S]*?<\/think>/gi, "");

  const openIdx = text.toLowerCase().indexOf("<think>");
  if (openIdx !== -1) {
    const chunk = text.substring(openIdx + 7).trim();
    if (chunk && thinking.indexOf(chunk) === -1) {
      thinking = thinking ? (thinking + "\n\n" + chunk) : chunk;
    }
    text = text.substring(0, openIdx);
  }
  const closeIdx = text.toLowerCase().indexOf("</think>");
  if (closeIdx !== -1) {
    const before = text.substring(0, closeIdx).trim();
    if (before && thinking.indexOf(before) === -1) {
      thinking = thinking ? (thinking + "\n\n" + before) : before;
    }
    text = text.substring(closeIdx + 8);
  }

  text = text.replace(/<\/?think>/gi, "").replace(/<\/?answer>/gi, "").trim();
  thinking = thinking.replace(/<\/?think>/gi, "").replace(/<\/?answer>/gi, "").trim();
  if (!text && thinking) text = thinking;
  return { thinking, cleanText: text };
}

async function copyTextWithFeedback(btnEl, textToCopy, defaultLabel) {
  try {
    if (navigator.clipboard && navigator.clipboard.writeText) {
      await navigator.clipboard.writeText(textToCopy);
    } else {
      const ta = document.createElement('textarea');
      ta.value = textToCopy;
      document.body.appendChild(ta);
      ta.select();
      document.execCommand('copy');
      ta.remove();
    }
    btnEl.textContent = tUi('copy.done');
    setTimeout(() => { btnEl.textContent = tUi(defaultLabel); }, 1600);
  } catch (e) {}
}

async function branchConversationAt(msgIndex, prefillPromptText) {
  if (activeGeneration || !currentChatId) return;
  try {
    const r = await fetch('/api/chats/branch', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({
        source_chat_id: currentChatId,
        message_index: msgIndex
      })
    });
    const d = await r.json();
    if (d.ok && d.id) {
      await openChatSession(d.id);
      if (prefillPromptText) {
        const inp = document.getElementById('promptInput');
        inp.value = prefillPromptText;
        inp.focus();
      }
    }
  } catch (e) {}
}

function updateAttachmentUI() {
  const strip = document.getElementById('attachmentStrip');
  const label = document.getElementById('attachmentLabel');
  if (attachedFileName) {
    strip.style.display = 'flex';
    label.textContent = tUi(attachedImageBase64 ? 'attachment.image' : 'attachment.file', {name:attachedFileName});
  } else {
    strip.style.display = 'none';
    label.textContent = '';
  }
}

function clearAttachment() {
  attachedImageBase64 = "";
  attachedTextContent = "";
  attachedFileName = "";
  document.getElementById('fileInput').value = "";
  updateAttachmentUI();
}

function handleSelectedFile(file) {
  if (!file) { clearAttachment(); return; }
  attachedFileName = file.name;
  attachedImageBase64 = "";
  attachedTextContent = "";
  if (file.type && file.type.startsWith('image/')) {
    const reader = new FileReader();
    reader.onload = () => {
      attachedImageBase64 = reader.result;
      updateAttachmentUI();
    };
    reader.readAsDataURL(file);
  } else {
    const reader = new FileReader();
    reader.onload = () => {
      attachedTextContent = reader.result || "";
      updateAttachmentUI();
    };
    reader.readAsText(file);
  }
}

document.getElementById('fileInput').addEventListener('change', function(e) {
  handleSelectedFile(e.target.files[0]);
});

let dragDepth = 0;
const dropOverlay = document.getElementById('dropOverlay');
window.addEventListener('dragenter', function(e) {
  e.preventDefault();
  dragDepth++;
  dropOverlay.classList.add('active');
});
window.addEventListener('dragover', function(e) {
  e.preventDefault();
});
window.addEventListener('dragleave', function(e) {
  e.preventDefault();
  dragDepth = Math.max(0, dragDepth - 1);
  if (dragDepth === 0) dropOverlay.classList.remove('active');
});
window.addEventListener('drop', function(e) {
  e.preventDefault();
  dragDepth = 0;
  dropOverlay.classList.remove('active');
  if (e.dataTransfer && e.dataTransfer.files && e.dataTransfer.files.length > 0) {
    handleSelectedFile(e.dataTransfer.files[0]);
  }
});

document.getElementById('promptInput').addEventListener('keydown', function(e) {
  if (e.key === 'Enter' && !e.shiftKey && !e.isComposing) { e.preventDefault(); sendPrompt(); }
});

async function refreshStats() {
  try {
    const r = await fetch('/api/system-stats');
    if (r.status === 401) return;
    const d = await r.json();
    document.getElementById('vramText').textContent = `${d.vram_used_mb} / ${d.vram_total_mb} MB (${d.vram_used_pct}%)`;
    document.getElementById('dramText').textContent = `${d.dram_used_mb} / ${d.dram_total_mb} MB (${d.dram_used_pct}%)`;
    document.getElementById('vramBar').style.width = `${Math.min(100, d.vram_used_pct)}%`;
    document.getElementById('dramBar').style.width = `${Math.min(100, d.dram_used_pct)}%`;
    document.getElementById('vramBar').style.background = d.vram_used_pct > 82 ? '#ef4444' : '#10b981';
    document.getElementById('dramBar').style.background = d.dram_used_pct > 80 ? '#f59e0b' : '#3b82f6';
    const badge = document.getElementById('activeModelBadge');
    const idle = !d.active_llm || d.active_llm.startsWith('None (');
    if (idle) badge.dataset.i18n = 'status.idle'; else delete badge.dataset.i18n;
    badge.textContent = idle ? tUi('status.idle') : d.active_llm;
  } catch (e) {}
}

async function purgeMemory() {
  await fetch('/api/purge-memory', { method: 'POST' });
  await refreshStats();
}

async function loadChatList() {
  try {
    const r = await fetch('/api/chats');
    if (r.status === 401) return;
    const d = await r.json();
    const list = document.getElementById('chatList');
    list.innerHTML = '';
    if (!d.chats || d.chats.length === 0) {
      const empty = document.createElement('div');
      empty.style.cssText = 'color:var(--faint);font-size:0.78rem;padding:8px;';
      empty.dataset.i18n = 'chats.empty'; empty.textContent = tUi('chats.empty');
      list.appendChild(empty);
      return;
    }
    for (const c of d.chats) {
      const item = document.createElement('div');
      item.className = 'chat-item' + (c.id === currentChatId ? ' active' : '');
      item.onclick = () => openChatSession(c.id);

      const title = document.createElement('div');
      title.className = 'chat-title';
      title.textContent = (c.parent_chat_id ? '🌿 ' : '') + (c.title || c.id);
      title.title = tUi('chats.count', {title:c.title || c.id, count:c.message_count || 0});
      item.appendChild(title);

      const del = document.createElement('button');
      del.className = 'chat-del';
      del.innerHTML = '&#128465;';
      del.title = tUi('chats.delete');
      del.onclick = async (ev) => {
        ev.stopPropagation();
        if (activeGeneration) return;
        await fetch('/api/chats/' + encodeURIComponent(c.id), { method: 'DELETE' });
        if (currentChatId === c.id) startNewChat();
        else await loadChatList();
      };
      item.appendChild(del);
      list.appendChild(item);
    }
  } catch (e) {}
}

function startNewChat() {
  if (activeGeneration) return;
  currentChatId = "";
  showWelcomeBanner();
  loadChatList();
}

async function openChatSession(id, fromGeneration = false) {
  if (activeGeneration && !fromGeneration) return;
  try {
    const r = await fetch('/api/chats/' + encodeURIComponent(id));
    const d = await r.json();
    if (!d.ok) return;
    currentChatId = d.id;
    const stream = document.getElementById('chatStream');
    stream.innerHTML = '';
    if (d.messages && d.messages.length > 0) {
      d.messages.forEach((m, idx) => {
        appendMessage(
          m.role || 'assistant',
          m.content || '',
          m.thinking || '',
          m.selected_tier || '',
          m.routing_reason || '',
          m.media_url || '',
          m.download_url || '',
          m.kind || '',
          m.attached_name || '',
          idx
        );
      });
    } else {
      showWelcomeBanner();
    }
    await loadChatList();
  } catch (e) {}
}

function appendMessage(role, text, thinkingText, tier, reason, mediaUrl, downloadUrl, kind, attachedName, msgIndex) {
  const stream = document.getElementById('chatStream');
  const div = document.createElement('div');
  div.className = 'msg ' + role;

  if (tier) {
    const tag = document.createElement('div');
    tag.className = 'route-tag';
    tag.textContent = tier === 'ROUTING' ? tUi('status.routing') : (reason ? `${tier} • ${reason}` : tier);
    if (tier === 'ROUTING') tag.dataset.i18n = 'status.routing';
    div.appendChild(tag);
  }
  if (attachedName) {
    const att = document.createElement('div');
    att.className = 'attach-badge';
    att.dataset.i18n = 'attachment.badge'; att.dataset.i18nVars = JSON.stringify({name:attachedName});
    att.textContent = tUi('attachment.badge', {name:attachedName});
    div.appendChild(att);
  }

  const parsed = (role === 'assistant')
    ? separateThinkingClientSide(text, thinkingText)
    : { thinking: "", cleanText: text };

  if (role === 'assistant' && parsed.thinking) {
    const details = document.createElement('details');
    details.className = 'think-box';
    const summary = document.createElement('summary');
    summary.dataset.i18n = 'thinking.show';
    summary.textContent = tUi('thinking.show');
    details.appendChild(summary);
    const thinkBody = document.createElement('div');
    thinkBody.className = 'think-content';
    thinkBody.textContent = parsed.thinking;
    details.appendChild(thinkBody);
    div.appendChild(details);
  }

  const body = document.createElement('div');
  body.className = (role === 'assistant') ? 'final-output' : '';
  body.textContent = parsed.cleanText;
  div.appendChild(body);

  if (mediaUrl) {
    if (kind === 'image') {
      const img = document.createElement('img');
      img.src = mediaUrl;
      div.appendChild(img);
    } else if (kind === 'video') {
      const vid = document.createElement('video');
      vid.src = mediaUrl; vid.controls = true; vid.autoplay = true; vid.loop = true;
      div.appendChild(vid);
    } else if (kind === 'audio') {
      const aud = document.createElement('audio');
      aud.src = mediaUrl; aud.controls = true; aud.style.marginTop = '10px'; aud.style.width = '100%';
      div.appendChild(aud);
    }
  }

  if (tier !== 'ROUTING') {
    const bar = document.createElement('div');
    bar.className = 'action-bar';

    const copyBtn = document.createElement('button');
    copyBtn.type = 'button';
    copyBtn.className = 'act-btn';
    const copyLabel = (role === 'user') ? 'copy.prompt' : 'copy.result';
    copyBtn.dataset.i18n = copyLabel;
    copyBtn.textContent = tUi(copyLabel);
    copyBtn.onclick = () => copyTextWithFeedback(copyBtn, parsed.cleanText, copyLabel);
    bar.appendChild(copyBtn);

    if (typeof msgIndex === 'number' && msgIndex >= 0) {
      const branchBtn = document.createElement('button');
      branchBtn.type = 'button';
      branchBtn.className = 'act-btn';
      branchBtn.dataset.i18n = 'branch.here';
    branchBtn.textContent = tUi('branch.here');
      branchBtn.dataset.i18nTitle = 'branch.tooltip';
    branchBtn.title = tUi('branch.tooltip');
      branchBtn.onclick = () => branchConversationAt(msgIndex, '');
      bar.appendChild(branchBtn);

      if (role === 'user') {
        const editBranchBtn = document.createElement('button');
        editBranchBtn.type = 'button';
        editBranchBtn.className = 'act-btn';
        editBranchBtn.dataset.i18n = 'branch.edit';
    editBranchBtn.textContent = tUi('branch.edit');
        editBranchBtn.dataset.i18nTitle = 'branch.edit_tooltip';
    editBranchBtn.title = tUi('branch.edit_tooltip');
        editBranchBtn.onclick = () => {
          if (msgIndex > 0) branchConversationAt(msgIndex - 1, parsed.cleanText);
          else {
            startNewChat();
            const inp = document.getElementById('promptInput');
            inp.value = parsed.cleanText;
            inp.focus();
          }
        };
        bar.appendChild(editBranchBtn);
      }
    }

    const dlTarget = downloadUrl || (mediaUrl ? (mediaUrl + '?download=1') : '');
    if (role === 'assistant' && dlTarget) {
      const a = document.createElement('a');
      a.className = 'dl-link';
      a.href = dlTarget;
      const cleanName = dlTarget.split('?')[0].split('/').pop() || 'output';
      a.setAttribute('download', cleanName);
      a.dataset.i18n = 'download.file'; a.dataset.i18nVars = JSON.stringify({name:cleanName});
      a.textContent = tUi('download.file', {name:cleanName});
      bar.appendChild(a);
    }

    div.appendChild(bar);
  }

  stream.appendChild(div);
  stream.scrollTop = stream.scrollHeight;
  return div;
}

function updateGenerationControls() {
  const active = activeGeneration;
  const key = !active ? 'prompt.send' : active.stopping ? 'prompt.stopping' : 'prompt.stop';
  const button = document.getElementById('sendButton');
  const label = document.getElementById('sendButtonLabel');
  label.dataset.i18n = key;
  label.textContent = tUi(key);
  button.dataset.i18nAriaLabel = key;
  button.setAttribute('aria-label', tUi(key));
  button.classList.toggle('primary', !active);
  button.classList.toggle('danger', !!active);
  button.disabled = !!(active && active.stopping);
  if (active) {
    button.dataset.i18nTitle = 'prompt.stop_tooltip';
    button.title = tUi('prompt.stop_tooltip');
  } else {
    delete button.dataset.i18nTitle;
    button.removeAttribute('title');
  }
  document.getElementById('newChatButton').disabled = !!active;
  document.body.classList.toggle('generation-active', !!active);
}

function handleSendButton() {
  if (activeGeneration) stopGeneration();
  else sendPrompt();
}

async function stopGeneration() {
  const generation = activeGeneration;
  if (!generation || generation.stopping) return;
  generation.stopping = true;
  const output = generation.pending.querySelector('.final-output');
  output.dataset.i18n = 'request.stopping';
  output.textContent = tUi('request.stopping');
  updateGenerationControls();
  const controller = new AbortController();
  const timeout = setTimeout(() => controller.abort(), 10000);
  try {
    // Keep the original request open until the backend has stopped its engine.
    const response = await fetch('/api/generation/stop', {
      method:'POST', headers:{'Content-Type':'application/json'},
      body:JSON.stringify({request_id:generation.id}), signal:controller.signal
    });
    const data = await response.json();
    if (!response.ok || !data.ok) throw new Error(data.error || String(response.status));
    if (activeGeneration !== generation) return;
    if (!data.cancelled) {
      // Completion already won the race; let the original response display.
      generation.stopping = false;
      output.dataset.i18n = 'request.pending';
      output.textContent = tUi('request.pending');
      updateGenerationControls();
    }
  } catch (error) {
    if (activeGeneration !== generation) return;
    generation.stopping = false;
    output.dataset.i18n = 'request.stop_failed';
    output.dataset.i18nVars = JSON.stringify({error:String(error)});
    output.textContent = tUi('request.stop_failed', {error:String(error)});
    updateGenerationControls();
  } finally {
    clearTimeout(timeout);
  }
}

async function sendPrompt() {
  if (activeGeneration) return;
  const input = document.getElementById('promptInput');
  let userText = input.value.trim();
  if (!userText && !attachedImageBase64 && !attachedTextContent) return;
  input.value = '';

  const mode = document.getElementById('taskMode').value;
  const reasoning = document.getElementById('reasoningToggle').checked;
  const uncensored = document.getElementById('uncensoredToggle').checked;
  const tier = document.getElementById('tierSelect').value;
  const webSearch = document.getElementById('webSearchToggle').checked;

  let fullPrompt = userText;
  if (attachedTextContent) {
    const header = `[Attached File: ${attachedFileName}]\n\`\`\`\n${attachedTextContent}\n\`\`\`\n\n`;
    fullPrompt = header + (userText || tUi('prompt.analyze_file'));
  } else if (!fullPrompt && attachedImageBase64) {
    fullPrompt = tUi('prompt.analyze_image');
  }

  const shownPrompt = userText || (attachedFileName ? tUi('prompt.attached', {name:attachedFileName}) : fullPrompt);
  const usedAttachmentName = attachedFileName;
  const usedImageBase64 = attachedImageBase64;
  clearAttachment();

  appendMessage('user', shownPrompt, '', '', '', '', '', '', usedAttachmentName, -1);
  const pending = appendMessage('assistant', tUi('request.pending'), '', 'ROUTING', '', '', '', '', '', -1);
  pending.querySelector('.final-output').dataset.i18n = 'request.pending';
  const requestBytes = new Uint8Array(16);
  crypto.getRandomValues(requestBytes);
  const generation = {id:Array.from(requestBytes, b => b.toString(16).padStart(2, '0')).join(''), pending, stopping:false};
  activeGeneration = generation;
  updateGenerationControls();

  try {
    let endpoint = '/api/chat';
    let payload = {
      request_id: generation.id,
      prompt: fullPrompt,
      task_mode: mode,
      thinking: reasoning,
      uncensored: uncensored,
      web_search: webSearch,
      tier: tier,
      image_base64: usedImageBase64,
      chat_id: currentChatId,
      attached_name: usedAttachmentName
    };
    if (mode === 'image') endpoint = '/api/generate-image';
    else if (mode === 'video') endpoint = '/api/generate-video';
    else if (mode === 'music') { endpoint = '/api/generate-music'; payload.tags = fullPrompt; }

    const resp = await fetch(endpoint, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(payload)
    });
    if (resp.status === 401) {
      pending.remove();
      await checkAuthStatus();
      return;
    }
    const data = await resp.json();
    pending.remove();
    if (data.cancelled) {
      const stopped = appendMessage('assistant', tUi('request.stopped'), '', '', '', '', '', '', '', -1);
      stopped.querySelector('.final-output').dataset.i18n = 'request.stopped';
    } else if (data.chat_id) {
      currentChatId = data.chat_id;
      await openChatSession(currentChatId, true);
    } else {
      appendMessage(
        'assistant',
        data.reply || data.error || tUi('request.empty'),
        data.thinking || '',
        data.selected_tier,
        data.routing_reason,
        data.media_url,
        data.download_url,
        data.kind,
        '',
        0
      );
    }
    await refreshStats();
  } catch (err) {
    pending.remove();
    appendMessage('assistant', tUi('request.failed', {error:String(err)}), '', 'ERROR', '', '', '', '', '', -1);
  } finally {
    if (activeGeneration === generation) {
      activeGeneration = null;
      updateGenerationControls();
      input.focus();
    }
  }
}

(async function bootstrapApp() {
  await initializeUiLanguage();
  showWelcomeBanner();
  const authed = await checkAuthStatus();
  if (authed) {
    await loadChatList();
    await refreshStats();
  }
  setInterval(refreshStats, 2500);
})();
</script>
</body>
</html>
)HTML";

} // namespace lais
