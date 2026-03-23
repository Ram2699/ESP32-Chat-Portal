/*
 * ESP32 Chat Portal – Complete Implementation (FINAL)
 * 
 * Features:
 * - Wi-Fi Access Point with captive portal
 * - User registration (unique username, password in RAM)
 * - Real‑time chat via WebSockets
 * - Admin page (separate login) with full control:
 *     kick (with ban timer), mute all, clear chat, edit any message,
 *     change Wi-Fi password, shut down portal, edit usernames, etc.
 * - Messages stored only in RAM (max 50 messages, 50 chars each)
 * - Max 8 concurrent users (rejects new connections beyond that)
 * - No persistent storage – everything lost on power cycle
 */

#include <WiFi.h>
#include <DNSServer.h>
#include <WebServer.h>
#include <WebSocketsServer.h>
#include <ArduinoJson.h>

// ==================== CONFIGURATION ====================
const char* ap_ssid = "ESP-WiFi Name";               // Change this
const char* ap_password = "ESP-WiFi Password";          // Change this
const char* admin_pin = "Admin Password";            // Change this

const int max_users = 8;                       // Max simultaneous users
const int max_messages = 50;                   // Max messages stored in RAM
const int max_msg_len = 50;                    // Max characters per message
const int ws_port = 81;                        // WebSocket server port
const int http_port = 80;                      // HTTP server port

// Timing
const unsigned long ban_duration_default = 5;  // Default kick ban minutes

// ==================== GLOBAL OBJECTS ====================
DNSServer dnsServer;
WebServer server(http_port);
WebSocketsServer webSocket = WebSocketsServer(ws_port);

// ==================== DATA STRUCTURES ====================
struct User {
  uint8_t clientId;
  String username;
  String password;
  IPAddress ip;
  bool isAdmin;
  unsigned long lastActivity;
};
User users[max_users];
int userCount = 0;

struct Ban {
  IPAddress ip;
  unsigned long expiry;
};
Ban bans[max_users];
int banCount = 0;

struct Message {
  unsigned int id;
  String username;
  String content;
  unsigned long timestamp;
};
Message messages[max_messages];
int messageCount = 0;
unsigned int nextMessageId = 1;

bool chatEnabled = true;
bool muteAll = false;
int adminClientId = -1;
bool portalShutdown = false;

// ==================== HELPER FUNCTIONS ====================
void addMessage(const String& username, const String& content) {
  if (messageCount >= max_messages) {
    for (int i = 1; i < max_messages; i++) {
      messages[i-1] = messages[i];
    }
    messageCount = max_messages - 1;
  }
  messages[messageCount].id = nextMessageId++;
  messages[messageCount].username = username;
  messages[messageCount].content = content.substring(0, max_msg_len);
  messages[messageCount].timestamp = millis();
  messageCount++;
}

void broadcastMessage(const String& type, const JsonObject& data, int excludeId = -1) {
  StaticJsonDocument<512> doc;
  doc["type"] = type;
  doc["data"] = data;
  String output;
  serializeJson(doc, output);
  for (int i = 0; i < userCount; i++) {
    if (users[i].clientId != excludeId && webSocket.clientIsConnected(users[i].clientId)) {
      webSocket.sendTXT(users[i].clientId, output);
    }
  }
}

void sendToClient(int clientId, const String& type, const JsonObject& data) {
  StaticJsonDocument<512> doc;
  doc["type"] = type;
  doc["data"] = data;
  String output;
  serializeJson(doc, output);
  webSocket.sendTXT(clientId, output);
}

int findUserByClientId(uint8_t clientId) {
  for (int i = 0; i < userCount; i++) {
    if (users[i].clientId == clientId) return i;
  }
  return -1;
}

int findUserByUsername(const String& username) {
  for (int i = 0; i < userCount; i++) {
    if (users[i].username == username) return i;
  }
  return -1;
}

void removeUser(int idx, bool broadcastLeave = true) {
  if (idx < 0 || idx >= userCount) return;
  if (users[idx].clientId == adminClientId) {
    adminClientId = -1;
  }
  if (broadcastLeave) {
    StaticJsonDocument<256> data;
    data["username"] = users[idx].username;
    broadcastMessage("user_left", data.as<JsonObject>());
  }
  webSocket.disconnect(users[idx].clientId);
  for (int i = idx; i < userCount - 1; i++) {
    users[i] = users[i+1];
  }
  userCount--;
}

void addBan(IPAddress ip, unsigned int minutes) {
  for (int i = 0; i < banCount; i++) {
    if (bans[i].ip == ip) {
      bans[i].expiry = millis() + minutes * 60000UL;
      return;
    }
  }
  if (banCount < max_users) {
    bans[banCount].ip = ip;
    bans[banCount].expiry = millis() + minutes * 60000UL;
    banCount++;
  }
}

bool isBanned(IPAddress ip) {
  unsigned long now = millis();
  int i = 0;
  while (i < banCount) {
    if (bans[i].expiry <= now) {
      for (int j = i; j < banCount - 1; j++) bans[j] = bans[j+1];
      banCount--;
    } else {
      i++;
    }
  }
  for (int i = 0; i < banCount; i++) {
    if (bans[i].ip == ip) return true;
  }
  return false;
}

void broadcastUserList() {
  StaticJsonDocument<1024> doc;
  doc["type"] = "user_list";
  JsonArray arr = doc.createNestedArray("users");
  for (int i = 0; i < userCount; i++) {
    arr.add(users[i].username);
  }
  String output;
  serializeJson(doc, output);
  for (int i = 0; i < userCount; i++) {
    webSocket.sendTXT(users[i].clientId, output);
  }
}

void sendMessageHistory(int clientId) {
  for (int i = 0; i < messageCount; i++) {
    StaticJsonDocument<256> data;
    data["id"] = messages[i].id;
    data["username"] = messages[i].username;
    data["content"] = messages[i].content;
    data["timestamp"] = messages[i].timestamp;
    sendToClient(clientId, "message", data.as<JsonObject>());
  }
}

// ==================== WEBSOCKET EVENT HANDLER ====================
void webSocketEvent(uint8_t num, WStype_t type, uint8_t * payload, size_t length) {
  switch(type) {
    case WStype_DISCONNECTED:
      {
        int idx = findUserByClientId(num);
        if (idx != -1) {
          removeUser(idx, true);
          broadcastUserList();
        }
      }
      break;
      
    case WStype_CONNECTED:
      {
        IPAddress ip = webSocket.remoteIP(num);
        if (portalShutdown) {
          webSocket.disconnect(num);
          return;
        }
        if (userCount >= max_users) {
          webSocket.sendTXT(num, "{\"type\":\"error\",\"data\":{\"message\":\"Chat is full. Try later.\"}}");
          webSocket.disconnect(num);
          return;
        }
        if (isBanned(ip)) {
          webSocket.sendTXT(num, "{\"type\":\"error\",\"data\":{\"message\":\"You are banned from this chat.\"}}");
          webSocket.disconnect(num);
          return;
        }
        StaticJsonDocument<256> doc;
        doc["type"] = "require_auth";
        String output;
        serializeJson(doc, output);
        webSocket.sendTXT(num, output);
      }
      break;
      
    case WStype_TEXT:
      {
        StaticJsonDocument<1024> doc;
        DeserializationError error = deserializeJson(doc, payload, length);
        if (error) return;
        
        String type = doc["type"];
        JsonObject data = doc["data"];
        
        if (type == "login") {
          String username = data["username"];
          String password = data["password"];
          
          if (username.length() < 4 || password.length() < 8) {
            webSocket.sendTXT(num, "{\"type\":\"error\",\"data\":{\"message\":\"Username must be ≥4 chars, password ≥8 chars\"}}");
            webSocket.disconnect(num);
            return;
          }
          
          if (findUserByUsername(username) != -1) {
            webSocket.sendTXT(num, "{\"type\":\"error\",\"data\":{\"message\":\"Username already taken\"}}");
            webSocket.disconnect(num);
            return;
          }
          
          bool isAdminUser = (username == "admin" && password == admin_pin);
          if (isAdminUser && adminClientId != -1) {
            webSocket.sendTXT(num, "{\"type\":\"error\",\"data\":{\"message\":\"Admin already logged in elsewhere\"}}");
            webSocket.disconnect(num);
            return;
          }
          
          users[userCount].clientId = num;
          users[userCount].username = username;
          users[userCount].password = password;
          users[userCount].ip = webSocket.remoteIP(num);
          users[userCount].isAdmin = isAdminUser;
          users[userCount].lastActivity = millis();
          userCount++;
          
          if (isAdminUser) {
            adminClientId = num;
          }
          
          StaticJsonDocument<256> confirm;
          confirm["type"] = "login_success";
          confirm["isAdmin"] = isAdminUser;
          String output;
          serializeJson(confirm, output);
          webSocket.sendTXT(num, output);
          
          StaticJsonDocument<256> joinData;
          joinData["username"] = username;
          broadcastMessage("user_joined", joinData.as<JsonObject>(), num);
          broadcastUserList();
          sendMessageHistory(num);
        }
        else if (type == "message") {
          int idx = findUserByClientId(num);
          if (idx == -1) {
            webSocket.sendTXT(num, "{\"type\":\"error\",\"data\":{\"message\":\"Not authenticated\"}}");
            webSocket.disconnect(num);
            return;
          }
          String content = data["content"];
          if (content.length() == 0) return;
          if (content.length() > max_msg_len) content = content.substring(0, max_msg_len);
          
          if (!chatEnabled && !users[idx].isAdmin) {
            webSocket.sendTXT(num, "{\"type\":\"error\",\"data\":{\"message\":\"Chat is disabled by admin\"}}");
            return;
          }
          if (muteAll && !users[idx].isAdmin) {
            webSocket.sendTXT(num, "{\"type\":\"error\",\"data\":{\"message\":\"Chat is muted by admin\"}}");
            return;
          }
          
          addMessage(users[idx].username, content);
          StaticJsonDocument<256> msgData;
          msgData["id"] = messages[messageCount-1].id;
          msgData["username"] = users[idx].username;
          msgData["content"] = content;
          msgData["timestamp"] = messages[messageCount-1].timestamp;
          broadcastMessage("message", msgData.as<JsonObject>());
        }
        else if (type == "admin_action") {
          int idx = findUserByClientId(num);
          if (idx == -1 || !users[idx].isAdmin) {
            webSocket.sendTXT(num, "{\"type\":\"error\",\"data\":{\"message\":\"Unauthorized\"}}");
            return;
          }
          
          String action = data["action"];
          if (action == "kick") {
            String targetUsername = data["username"];
            int targetIdx = findUserByUsername(targetUsername);
            if (targetIdx != -1 && !users[targetIdx].isAdmin) {
              int minutes = data["minutes"] | ban_duration_default;
              addBan(users[targetIdx].ip, minutes);
              removeUser(targetIdx, true);
              broadcastUserList();
              webSocket.sendTXT(num, "{\"type\":\"admin_result\",\"data\":{\"success\":true}}");
            } else {
              webSocket.sendTXT(num, "{\"type\":\"admin_result\",\"data\":{\"success\":false,\"message\":\"User not found or cannot kick admin\"}}");
            }
          }
          else if (action == "mute") {
            muteAll = data["enabled"];
            broadcastMessage("mute_status", data);
            webSocket.sendTXT(num, "{\"type\":\"admin_result\",\"data\":{\"success\":true}}");
          }
          else if (action == "clear") {
            messageCount = 0;
            nextMessageId = 1;
            broadcastMessage("clear_all", JsonObject());
            webSocket.sendTXT(num, "{\"type\":\"admin_result\",\"data\":{\"success\":true}}");
          }
          else if (action == "edit_message") {
            unsigned int msgId = data["id"];
            String newContent = data["new_content"];
            newContent = newContent.substring(0, max_msg_len);
            for (int i = 0; i < messageCount; i++) {
              if (messages[i].id == msgId) {
                messages[i].content = newContent;
                StaticJsonDocument<256> editData;
                editData["id"] = msgId;
                editData["new_content"] = newContent;
                broadcastMessage("message_edited", editData.as<JsonObject>());
                break;
              }
            }
            webSocket.sendTXT(num, "{\"type\":\"admin_result\",\"data\":{\"success\":true}}");
          }
          else if (action == "edit_username") {
            String oldName = data["old_username"];
            String newName = data["new_username"];
            int targetIdx = findUserByUsername(oldName);
            if (targetIdx != -1 && !users[targetIdx].isAdmin) {
              if (findUserByUsername(newName) == -1) {
                users[targetIdx].username = newName;
                StaticJsonDocument<256> renameData;
                renameData["old"] = oldName;
                renameData["new"] = newName;
                broadcastMessage("user_renamed", renameData.as<JsonObject>());
                broadcastUserList();
                webSocket.sendTXT(num, "{\"type\":\"admin_result\",\"data\":{\"success\":true}}");
              } else {
                webSocket.sendTXT(num, "{\"type\":\"admin_result\",\"data\":{\"success\":false,\"message\":\"New username already exists\"}}");
              }
            } else {
              webSocket.sendTXT(num, "{\"type\":\"admin_result\",\"data\":{\"success\":false,\"message\":\"User not found or cannot rename admin\"}}");
            }
          }
          else if (action == "disable_chat") {
            chatEnabled = data["enabled"];
            broadcastMessage("chat_enabled", data);
            webSocket.sendTXT(num, "{\"type\":\"admin_result\",\"data\":{\"success\":true}}");
          }
          else if (action == "change_wifi_password") {
            String newPassword = data["new_password"];
            if (newPassword.length() >= 8) {
              WiFi.softAP(ap_ssid, newPassword.c_str());
              webSocket.sendTXT(num, "{\"type\":\"admin_result\",\"data\":{\"success\":true,\"message\":\"Wi-Fi password changed. Reconnect with new password.\"}}");
            } else {
              webSocket.sendTXT(num, "{\"type\":\"admin_result\",\"data\":{\"success\":false,\"message\":\"Password must be at least 8 chars\"}}");
            }
          }
          else if (action == "shutdown_portal") {
            portalShutdown = true;
            webSocket.sendTXT(num, "{\"type\":\"admin_result\",\"data\":{\"success\":true,\"message\":\"Portal shutting down...\"}}");
            delay(1000);
            WiFi.softAPdisconnect(true);
            server.stop();
            webSocket.disconnect();
            while(1) delay(1000);
          }
          else {
            webSocket.sendTXT(num, "{\"type\":\"admin_result\",\"data\":{\"success\":false,\"message\":\"Unknown action\"}}");
          }
        }
      }
      break;
      
    default:
      break;
  }
}

// ==================== HTTP HANDLERS ====================
void handleRoot() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>ESP32 Chat Login</title>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        body { font-family: Arial; background: #121212; color: #eee; display: flex; justify-content: center; align-items: center; height: 100vh; margin: 0; }
        .container { background: #1e1e1e; padding: 2rem; border-radius: 10px; width: 300px; }
        input { width: 100%; padding: 8px; margin: 8px 0; background: #2c2c2c; border: none; color: white; border-radius: 4px; }
        button { width: 100%; padding: 10px; background: #6200ea; border: none; color: white; border-radius: 4px; cursor: pointer; }
        button:hover { background: #3700b3; }
        .error { color: #cf6679; margin-top: 10px; text-align: center; }
        h2 { text-align: center; }
    </style>
</head>
<body>
    <div class="container">
        <h2>Chat Login</h2>
        <input type="text" id="username" placeholder="Username (min 4 chars)" autocomplete="off">
        <input type="password" id="password" placeholder="Password (min 8 chars)">
        <button onclick="login()">Login</button>
        <div id="error" class="error"></div>
    </div>
    <script>
        function login() {
            let username = document.getElementById('username').value.trim();
            let password = document.getElementById('password').value;
            if (username.length < 4 || password.length < 8) {
                document.getElementById('error').innerText = 'Invalid credentials';
                return;
            }
            let ws = new WebSocket('ws://' + window.location.hostname + ':81');
            ws.onopen = function() {
                ws.send(JSON.stringify({type: 'login', data: {username: username, password: password}}));
            };
            ws.onmessage = function(e) {
                let msg = JSON.parse(e.data);
                if (msg.type === 'login_success') {
                    sessionStorage.setItem('chat_username', username);
                    sessionStorage.setItem('chat_password', password);
                    sessionStorage.setItem('chat_isAdmin', msg.isAdmin);
                    window.location.href = '/chat';
                } else if (msg.type === 'error') {
                    document.getElementById('error').innerText = msg.data.message;
                    ws.close();
                } else {
                    // Unexpected
                }
            };
            ws.onerror = function() {
                document.getElementById('error').innerText = 'Connection error';
            };
        }
    </script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", html);
}

void handleChat() {
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
    <title>ESP32 Chat</title>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1">
    <style>
        * { box-sizing: border-box; }
        body { font-family: Arial; background: #121212; color: #eee; margin: 0; padding: 0; }
        #chat-container { display: flex; flex-direction: column; height: 100vh; }
        #messages { flex: 1; overflow-y: auto; padding: 10px; display: flex; flex-direction: column; }
        .message { margin: 5px; padding: 8px; border-radius: 8px; max-width: 70%; word-wrap: break-word; }
        .message.own { background: #6200ea; align-self: flex-end; }
        .message.other { background: #2c2c2c; align-self: flex-start; }
        .message .username { font-weight: bold; font-size: 0.8em; margin-bottom: 4px; }
        .message .content { font-size: 1em; }
        .message .timestamp { font-size: 0.7em; color: #aaa; text-align: right; margin-top: 4px; }
        #input-area { display: flex; padding: 10px; background: #1e1e1e; border-top: 1px solid #333; }
        #message-input { flex: 1; padding: 8px; background: #2c2c2c; border: none; color: white; border-radius: 4px; }
        #send-btn { margin-left: 8px; padding: 8px 16px; background: #6200ea; border: none; color: white; border-radius: 4px; cursor: pointer; }
        #user-list { position: fixed; right: 10px; top: 10px; background: #1e1e1e; padding: 10px; border-radius: 8px; width: 150px; font-size: 0.8em; }
        #user-list h4 { margin: 0 0 5px 0; }
        #user-list div { padding: 2px; }
        .admin-panel { position: fixed; left: 10px; top: 10px; background: #1e1e1e; padding: 10px; border-radius: 8px; width: 200px; font-size: 0.8em; display: none; }
        .admin-panel button { width: 100%; margin: 2px 0; }
    </style>
</head>
<body>
<div id="chat-container">
    <div id="messages"></div>
    <div id="input-area">
        <input type="text" id="message-input" placeholder="Type a message..." maxlength="50">
        <button id="send-btn">Send</button>
    </div>
</div>
<div id="user-list"><h4>Online</h4><div id="user-list-content"></div></div>
<div id="admin-panel" class="admin-panel"><h4>Admin Controls</h4><div id="admin-controls"></div></div>

<script>
    let ws;
    let username = sessionStorage.getItem('chat_username');
    let password = sessionStorage.getItem('chat_password');
    let isAdmin = sessionStorage.getItem('chat_isAdmin') === 'true';

    if (!username || !password) {
        alert('Session expired, please login again.');
        window.location.href = '/';
    }

    function connect() {
        ws = new WebSocket('ws://' + window.location.hostname + ':81');
        ws.onopen = function() {
            ws.send(JSON.stringify({type: 'login', data: {username: username, password: password}}));
        };
        ws.onmessage = function(e) {
            let msg = JSON.parse(e.data);
            if (msg.type === 'login_success') {
                if (isAdmin) document.getElementById('admin-panel').style.display = 'block';
                if (isAdmin) loadAdminControls();
            } else if (msg.type === 'message') {
                addMessageToChat(msg.data.username, msg.data.content, msg.data.timestamp, msg.data.id, msg.data.username === username);
            } else if (msg.type === 'user_joined') {
                addSystemMessage(msg.data.username + ' joined');
                updateUserList();
            } else if (msg.type === 'user_left') {
                addSystemMessage(msg.data.username + ' left');
                updateUserList();
            } else if (msg.type === 'user_list') {
                let listDiv = document.getElementById('user-list-content');
                listDiv.innerHTML = '';
                msg.users.forEach(u => {
                    let div = document.createElement('div');
                    div.textContent = u;
                    listDiv.appendChild(div);
                });
            } else if (msg.type === 'clear_all') {
                document.getElementById('messages').innerHTML = '';
            } else if (msg.type === 'message_edited') {
                let msgs = document.querySelectorAll('.message');
                for (let m of msgs) {
                    if (m.getAttribute('data-id') == msg.data.id) {
                        let contentSpan = m.querySelector('.content');
                        if (contentSpan) contentSpan.textContent = msg.data.new_content;
                        break;
                    }
                }
            } else if (msg.type === 'user_renamed') {
                addSystemMessage(msg.data.old + ' is now known as ' + msg.data.new);
                updateUserList();
            } else if (msg.type === 'mute_status') {
                addSystemMessage('Chat is now ' + (msg.data.enabled ? 'muted' : 'unmuted'));
            } else if (msg.type === 'chat_enabled') {
                addSystemMessage('Chat is now ' + (msg.data.enabled ? 'enabled' : 'disabled'));
            } else if (msg.type === 'error') {
                alert('Error: ' + msg.data.message);
                if (msg.data.message === 'Username already taken' || msg.data.message.includes('password')) {
                    sessionStorage.clear();
                    window.location.href = '/';
                }
            }
        };
        ws.onclose = function() {
            addSystemMessage('Disconnected. Reconnecting...');
            setTimeout(connect, 3000);
        };
    }

    function addMessageToChat(username, content, timestamp, id, isOwn) {
        let msgDiv = document.createElement('div');
        msgDiv.className = 'message ' + (isOwn ? 'own' : 'other');
        msgDiv.setAttribute('data-id', id);
        let usernameSpan = document.createElement('div');
        usernameSpan.className = 'username';
        usernameSpan.textContent = username;
        let contentSpan = document.createElement('div');
        contentSpan.className = 'content';
        contentSpan.textContent = content;
        let timeSpan = document.createElement('div');
        timeSpan.className = 'timestamp';
        timeSpan.textContent = new Date(timestamp).toLocaleTimeString();
        msgDiv.appendChild(usernameSpan);
        msgDiv.appendChild(contentSpan);
        msgDiv.appendChild(timeSpan);
        document.getElementById('messages').appendChild(msgDiv);
        msgDiv.scrollIntoView({behavior: 'smooth', block: 'end'});
    }

    function addSystemMessage(text) {
        let sysDiv = document.createElement('div');
        sysDiv.style.textAlign = 'center';
        sysDiv.style.fontSize = '0.8em';
        sysDiv.style.color = '#aaa';
        sysDiv.style.margin = '5px';
        sysDiv.textContent = text;
        document.getElementById('messages').appendChild(sysDiv);
        sysDiv.scrollIntoView({behavior: 'smooth', block: 'end'});
    }

    function updateUserList() {
        // The user_list event will update it; we don't need to do anything extra.
    }

    function loadAdminControls() {
        let controlsDiv = document.getElementById('admin-controls');
        controlsDiv.innerHTML = `
            <button onclick="adminKick()">Kick User</button>
            <button onclick="adminMute()">Toggle Mute</button>
            <button onclick="adminClear()">Clear Chat</button>
            <button onclick="adminEditMessage()">Edit Message</button>
            <button onclick="adminEditUsername()">Edit Username</button>
            <button onclick="adminDisableChat()">Toggle Chat</button>
            <button onclick="adminChangeWifiPassword()">Change WiFi Password</button>
            <button onclick="adminShutdown()">Shutdown Portal</button>
        `;
    }

    function adminKick() {
        let user = prompt('Enter username to kick:');
        if (user) {
            let minutes = prompt('Ban minutes (default 5):', 5);
            if (minutes !== null) {
                ws.send(JSON.stringify({type: 'admin_action', data: {action: 'kick', username: user, minutes: parseInt(minutes)}}));
            }
        }
    }
    function adminMute() {
        let enabled = confirm('Mute chat?');
        ws.send(JSON.stringify({type: 'admin_action', data: {action: 'mute', enabled: enabled}}));
    }
    function adminClear() {
        if (confirm('Clear all messages?')) {
            ws.send(JSON.stringify({type: 'admin_action', data: {action: 'clear'}}));
        }
    }
    function adminEditMessage() {
        let id = prompt('Message ID to edit:');
        if (id) {
            let newContent = prompt('New content:');
            if (newContent) {
                ws.send(JSON.stringify({type: 'admin_action', data: {action: 'edit_message', id: parseInt(id), new_content: newContent}}));
            }
        }
    }
    function adminEditUsername() {
        let old = prompt('Current username:');
        if (old) {
            let newName = prompt('New username:');
            if (newName) {
                ws.send(JSON.stringify({type: 'admin_action', data: {action: 'edit_username', old_username: old, new_username: newName}}));
            }
        }
    }
    function adminDisableChat() {
        let enabled = confirm('Disable chat?');
        ws.send(JSON.stringify({type: 'admin_action', data: {action: 'disable_chat', enabled: enabled}}));
    }
    function adminChangeWifiPassword() {
        let newPwd = prompt('New Wi-Fi password (min 8 chars):');
        if (newPwd && newPwd.length >= 8) {
            ws.send(JSON.stringify({type: 'admin_action', data: {action: 'change_wifi_password', new_password: newPwd}}));
        } else alert('Password must be at least 8 chars');
    }
    function adminShutdown() {
        if (confirm('Shutdown portal? This will stop the ESP32 service.')) {
            ws.send(JSON.stringify({type: 'admin_action', data: {action: 'shutdown_portal'}}));
        }
    }

    connect();

    document.getElementById('send-btn').onclick = function() {
        let input = document.getElementById('message-input');
        let msg = input.value.trim();
        if (msg) {
            ws.send(JSON.stringify({type: 'message', data: {content: msg}}));
            input.value = '';
        }
    };
    document.getElementById('message-input').addEventListener('keypress', function(e) {
        if (e.key === 'Enter') document.getElementById('send-btn').click();
    });
</script>
</body>
</html>
)rawliteral";
  server.send(200, "text/html", html);
}

void handleAdmin() {
  server.send(200, "text/html", "<html><body>Admin panel is integrated into chat page if you log in as admin.</body></html>");
}

void handleNotFound() {
  server.sendHeader("Location", String("http://") + WiFi.softAPIP().toString(), true);
  server.send(302, "text/plain", "");
}

// ==================== SETUP ====================
void setup() {
  Serial.begin(115200);
  
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ap_ssid, ap_password);
  IPAddress myIP = WiFi.softAPIP();
  Serial.println("AP IP address: " + myIP.toString());
  
  dnsServer.start(53, "*", myIP);
  
  server.on("/", handleRoot);
  server.on("/chat", handleChat);
  server.on("/admin", handleAdmin);
  server.onNotFound(handleNotFound);
  server.begin();
  
  webSocket.begin();
  webSocket.onEvent(webSocketEvent);
  
  Serial.println("Chat portal started. Connect to Wi-Fi: " + String(ap_ssid) + " with password: " + String(ap_password));
}

// ==================== LOOP ====================
void loop() {
  dnsServer.processNextRequest();
  server.handleClient();
  webSocket.loop();
}