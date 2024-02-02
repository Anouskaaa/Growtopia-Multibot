#define _SILENCE_ALL_MS_EXT_DEPRECATION_WARNINGS

#include "Bot.h"

#include <chrono>

#include <proton/Variant.h>

#include "../Utils/Random.h"
#include "../Utils/TextParse.h"
#include "../Utils/Hash.hpp"
#include "../Utils/Klv.hpp"
#include "../Packet/Packet.h"

using namespace std::chrono_literals;

void Bot::GenerateNewSpoof() {
    m_login_mac = Utils::Random::RandomMac();
    m_login_rid = Utils::Random::RandomHex(32, true);
    m_login_wk = Utils::Random::RandomHex(32, true);
    m_login_hash = std::to_string(Utils::Hash::proton(fmt::format("{}RT", Utils::Random::RandomHex(16, true)).c_str()));
}

void Bot::SetLoginGuest(std::string mac, std::string rid) {
    m_login_mac = mac;
    m_login_rid = rid;

    m_login_growid = "";
    m_login_growid_pass = "";
}

void Bot::SetLoginGrowID(std::string growid, std::string password) {
    m_login_growid = growid;
    m_login_growid_pass = password;
}

bool Bot::ConnectWithHttp() {
retry:

    m_logger->Info("Getting server data...");
    Utils::TextParse parse{ m_http_cl.Post("/growtopia/server_data.php", {}, {}, "application/x-www-form-urlencoded"), "\n" };

    if (parse.Get("server").empty()) {
        m_logger->Error("Failed to get server data. Retrying in 5 seconds");
        std::this_thread::sleep_for(5000ms);
        goto retry;
    }

    if (!Connect(parse.Get("server"), parse.Get("port"), parse.Get("meta"), true)) {
        m_logger->Error("Failed to connect to server. Reconnecting to server in 3 seconds");
        goto retry;
    }

    return true;
}

bool Bot::Connect(std::string ip, std::string port, std::string meta, bool use_new_packet) { 
    m_login_meta = meta;
    m_using_new_packet = use_new_packet;

    return Client::Connect(ip, port, use_new_packet);
}

void Bot::StartBotThread() {
    m_is_running = true;
    Client::StartThread();
    m_bot_thread = std::move(std::thread{ &Bot::bot_thread, this });
}

void Bot::StopBotThread() {
    m_is_running = false; 
    Client::StopThread();
    m_bot_thread.join();
}

void Bot::JoinWorld(std::string world)
{
    SendPacket({
        ePacketType::NET_MESSAGE_GAME_MESSAGE,
        fmt::format("action|join_request\nname|{}\ninvitedWorld|0", world)
    });
}

void Bot::Move(float x, float y) {
    m_pos_x += x * 32;
    m_pos_y += y * 32;
}

void Bot::bot_thread() {
    m_logger->Debug("STARTED BOT THREAD");

    ENetEvent event{};
    while (m_is_running) {
        if (m_last_pos_x == m_pos_x && m_last_pos_y == m_pos_y && m_is_bot_moving) {
            TankPacket tank_pkt{};

            tank_pkt.Header.Type = eTankPacketType::NET_GAME_PACKET_STATE;
            tank_pkt.Header.IntX = -1;
            tank_pkt.Header.IntY = -1;
            tank_pkt.Header.VectorX = m_pos_x;
            tank_pkt.Header.VectorY = m_pos_y;
            tank_pkt.Header.VectorX2 = 0;
            tank_pkt.Header.VectorY2 = 0;
            tank_pkt.Header.Flags.bUnk = true;
            tank_pkt.Header.Flags.bOnSolid = true;

            SendPacket(tank_pkt);
            m_is_bot_moving = false;
        }
        if (m_last_pos_x != m_pos_x || m_last_pos_y != m_pos_y) {
            TankPacket tank_pkt{};

            tank_pkt.Header.Type = eTankPacketType::NET_GAME_PACKET_STATE;
            tank_pkt.Header.VectorX = m_pos_x;
            tank_pkt.Header.VectorY = m_pos_y;
            tank_pkt.Header.Flags.bUnk = true; // idk why but this is present on alot of packets
            tank_pkt.Header.IntX = -1;
            tank_pkt.Header.IntY = -1;

            if (m_pos_x - m_last_pos_x < 0) {
                tank_pkt.Header.VectorX2 = -250;
                tank_pkt.Header.Flags.bRotateLeft = true;
            }
            else if (m_pos_x - m_last_pos_x > 0) {
                tank_pkt.Header.VectorX2 = 250;
                tank_pkt.Header.Flags.bRotateLeft = false;
            }

            if (m_pos_y - m_last_pos_y < 0) {
                tank_pkt.Header.VectorY2 = -250;
                tank_pkt.Header.Flags.bOnJump = true;
            }
            else if (m_pos_y - m_last_pos_y > 0) {
                tank_pkt.Header.VectorY2 = 250;
                tank_pkt.Header.Flags.bOnJump = true;
            }

            tank_pkt.Header.Flags.bOnSolid = true;

            SendPacket(tank_pkt);

            m_last_pos_x = m_pos_x;
            m_last_pos_y = m_pos_y;
            m_is_bot_moving = true;
        }
        std::this_thread::sleep_for(250ms);
    }

    m_logger->Debug("BOT THREAD ENDED");
}

void Bot::on_connect() {
    m_logger->Info("Bot connected to the server.");
}

void Bot::on_receive(ENetPacket* pkt) {
    Packet rec_packet { pkt };

    m_logger->LogString(ILogger::LogType::Info, "Incoming " + rec_packet.ToString());

    enet_packet_destroy(pkt);


    switch (rec_packet.Type)
    {
    case ePacketType::NET_MESSAGE_GAME_PACKET: {
        if (rec_packet.TankPkt->Header.Type == eTankPacketType::NET_GAME_PACKET_CALL_FUNCTION) {
            VariantList varlist {};
            varlist.SerializeFromMem(rec_packet.TankPkt->Value.data(), rec_packet.TankPkt->Value.size());
            
            on_incoming_varlist(varlist, *rec_packet.TankPkt);
            break;
        }
        on_incoming_tank_packet(*rec_packet.TankPkt);
        break;
    }
    default:
        on_incoming_packet(rec_packet.Type, *rec_packet.TextPkt);
        break;
    }
}

void Bot::on_disconnect() {
    m_logger->Info("Bot disconnected from the server.");

    if (m_is_redirected) {
        m_logger->Info("Reconnecting to sub-server");
        if (!Connect(m_server_ip, m_server_port, m_login_meta, m_using_new_packet)) {
            m_logger->Error("Error occured while reconnecting to sub-server");
        }
        m_is_redirected = false;
        return;
    }

    if (m_reconnect || m_always_reconnect) {
        m_logger->Info("Auto reconnecting in 3 seconds");
        std::this_thread::sleep_for(3000ms);
        ConnectWithHttp();
        m_reconnect = false;
    }

    m_is_in_game = false;
}

void Bot::on_login() {
    m_logger->Debug("building login data");
    Utils::TextParse login_data{ "requestedName|MouseGar", "\n"};

    if (!m_login_growid.empty()) {
        login_data = Utils::TextParse("tankIDName|"+m_login_growid, "\n");
        login_data.Add("tankIDPass", m_login_growid_pass);
        login_data.Add("requestedName", "MouseGar");
    }

    login_data.Add("f", "1");
    login_data.Add("protocol", m_game_proto_version);
    login_data.Add("game_version", m_game_version);
    login_data.Add("fz", "53717544");
    login_data.Add("cbits", "1024");
    login_data.Add("player_age", "19");
    login_data.Add("GDPR", "1");
    login_data.Add("category", "_-5000");
    login_data.Add("totalPlaytime", "0");
    login_data.Add("meta", m_login_meta);
    login_data.Add("platformID", "0,1,1");
    login_data.Add("deviceVersion", "0");
    login_data.Add("country", "us");
    login_data.Add("mac", m_login_mac);
    login_data.Add("wk", m_login_wk);
    login_data.Add("zf", "-1433566146");
    login_data.Add("hash", m_login_hash);
    login_data.Add("fhash", "-716928004");
    login_data.Add("rid", m_login_rid);
    login_data.Add("lmode", "0");

    if (!m_user_id.empty()) {
        login_data.Add("user", m_user_id);
        login_data.Add("UUIDToken", m_login_uuid_token);
        login_data.Add("token", m_login_token);
        login_data.Add("doorID", m_login_door_id);
        login_data.Set("lmode", "1");
    }

    login_data.Add("hash2", std::to_string(Utils::Hash::proton(fmt::format("{}RT", m_login_mac).c_str())));
    login_data.Add("klv", Utils::generate_klv(std::stoi(m_game_proto_version), m_game_version, m_login_rid));

    Packet login_pkt{ ePacketType::NET_MESSAGE_GENERIC_TEXT, login_data.GetTextRaw() };
    SendPacket(login_pkt.CreateToENetPacket());
    m_logger->Debug("Sent login data");
}

void Bot::on_login_fail() {
    m_logger->Error("Login failed.");
    m_reconnect = m_always_reconnect;
    Disconnect();
}

void Bot::on_incoming_packet(ePacketType type, TextPacket pkt) {
    switch (type)
    {
    case NET_MESSAGE_UNKNOWN:
        break;
    case NET_MESSAGE_SERVER_HELLO: {
        on_login();
        break;
    }
    case NET_MESSAGE_GENERIC_TEXT:
        break;
    case NET_MESSAGE_GAME_MESSAGE: {
        if (pkt == "action|logon_fail\n") {
            on_login_fail();
        }
        break;
    }
    case NET_MESSAGE_GAME_PACKET:
        break;
    case NET_MESSAGE_ERROR:
        break;
    case NET_MESSAGE_TRACK:
        break;
    case NET_MESSAGE_CLIENT_LOG_REQUEST:
        break;
    case NET_MESSAGE_CLIENT_LOG_RESPONSE:
        break;
    default:
        break;
    }
}

void Bot::on_incoming_tank_packet(TankPacket pkt) {
    if (pkt.Header.Type == eTankPacketType::NET_GAME_PACKET_PING_REQUEST) {
        TankPacket tank_pkt = {};

        //https://github.com/ama7nen/enetproxy/blob/62079ce101a0794dbe958790873533cb71b4fb08/proxy/events.cpp#L18
        tank_pkt.Header.Type = eTankPacketType::NET_GAME_PACKET_PING_REPLY;
        tank_pkt.Header.VectorX2 = 1000.f; // gravity
        tank_pkt.Header.VectorY2 = 250.f; // move speed
        tank_pkt.Header.VectorX = 64.f; // punch range
        tank_pkt.Header.VectorY = 64.f; // build range
        tank_pkt.Header.JumpCount = 0;

        SendPacket({ tank_pkt });
    }
}

void Bot::on_incoming_varlist(VariantList varlist, TankPacket pkt) {
    size_t hash = Utils::Hash::fnv1a(varlist.Get(0).GetString().c_str());

    switch (hash)
    {
    case "OnSendToServer"_fh :{
        on_redirect(&varlist);
        break;
    }
    case "OnReconnect"_fh: {
        m_reconnect = true;
        Disconnect();
        break;
    }
    case "OnSuperMainStartAcceptLogonHrdxs47254722215a"_fh: {
        m_is_in_game = true;
        SendPacket(Packet{ePacketType::NET_MESSAGE_GENERIC_TEXT ,"action|enter_game"});
        break;
    }
    case "OnSetPos"_fh: {
        CL_Vec2f pos = varlist.Get(1).GetVector2();
        m_pos_x = pos.x;
        m_pos_y = pos.y;
        break;
    }
    default:
        break;
    }
}

void Bot::on_redirect(VariantList* varlist) {
    m_is_redirected = true;

    m_logger->Debug("Got server redirect");

    Disconnect();

    std::vector<std::string> redirect_server_data{ Utils::TextParse::StringTokenize(varlist->Get(4).GetString(), "|")};
    m_login_door_id = redirect_server_data.at(1);
    m_login_uuid_token = redirect_server_data.at(2);
    m_login_token = std::to_string(varlist->Get(2).GetINT32());
    m_user_id = std::to_string(varlist->Get(3).GetINT32());
     
    m_server_ip = redirect_server_data.at(0);
    m_server_port = std::to_string(varlist->Get(1).GetINT32());
}