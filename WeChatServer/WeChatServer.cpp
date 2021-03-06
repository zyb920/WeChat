/*
 * WeChatServer.cpp
 *
 *  Created on: 2015-02-18 16:16
 *      Author: young
 *
 *  POWERED BY ENET
 *  E-MAIL: xmulyj@gmail.com
 */
#include "WeChatServer.h"
#include <sstream>

/* 1: 10000以内的CmdID内部占用,请从10001开始定义自己的CmdID;
 * 2: 建议将cmd ID移到统一的文件中进行定义;
 */

IMPL_LOGGER(WeChatServer, logger);

int WeChatServer::OnInit(ConfReader *config)
{
    //注册Cmd处理方法
    CALL_HANDLE_REG();

    //添加其他初始化内容
    SetServer(this);

    //初始化DB
    string db_path = "../db/UserInfoDB";
    m_DBWrap = new RocksDBWrap(db_path);
    m_DBWrap->Init();

    return 0;
}

bool WeChatServer::OnPacket(TCPSession *session, uint32_t cmd, const char *packet_data, uint32_t head_size, uint32_t body_size, uint64_t tid)
{
    if(!HAS_HANDLE(cmd))
    {
        LOG_TRACE(logger, "can't find handler for cmd="<<cmd<<".fd="<<session->GetFD()<<",pass to parent class");
        //由父类来处理
        return TCPServer::OnPacket(session, cmd, packet_data, head_size, body_size, tid);
    }

    HANLDER_PTR handler = GET_HANDLE(cmd);
    int ret = (this->*handler)(session, packet_data, head_size, body_size, tid);
    return ret==0?true:false;
}

// 需要处理错误,请去掉注释
IOStatus WeChatServer::OnError(TCPSession *session, uint64_t now_ms)
{
    //处理错误
    LOG_DEBUG(logger, "OnError:remove session from UserInfoMap.session="<<session);

    SessionUserInfoMap::iterator it = m_SessionUserInfoMap.find(session);
    if(it != m_SessionUserInfoMap.end())
    {
        //通知状态服务器(不需要回包)
        NotifyUserStatus("OnError", 2, it->second->id(), 1);
    }

    m_SessionUserInfoMap.erase(session);

    return TCPServer::OnError(session, now_ms);
}


/* 需要处理自己的监听,请去掉注释
void WeChatServer::OnListenSucc(ListenInfo &listen_info, const ConfSessionParam &session_param)
{
    return ;
}
*/

/* 需要处理每秒检查任务,请去掉注释
void WeChatServer::OnCheckPerSec(uint64_t now_ms)
{
    return ;
}
*/

#if USE_EXAMPLE == 1
int WeChatServer::OnExample(TCPSession *session, const char *data, uint32_t head_size, uint32_t body_size, uint64_t tid)
{
    string req_str;
    req_str.assign(data+head_size, body_size);
    LOG_INFO(logger, "OnExample|Req="<<req_str);

    string rsp_str = "TestExample is ok!";
    char buffer[1024];
    uint32_t rsp_size = 0;

    Packet *packet = GetInternalPacket();
    int header_size = packet->SetHead(buffer, sizeof(buffer), rsp_str.size(), CMD_EXAMPLE_RSP);
    assert(header_size > 0);

    memcpy(buffer+header_size, rsp_str.c_str(), rsp_str.size());
    rsp_size = header_size+rsp_str.size();

    if(session->AsyncSend(buffer, rsp_size) == true)
    {
        LOG_INFO(logger, "OnExample|send OnExampleRsp succ.fd="<<session->GetFD()<<".rsp_size="<<rsp_size);
    }
    else
    {
        LOG_WARN(logger, "OnExample|send OnExampleRsp failed.fd="<<session->GetFD()<<".rsp_size="<<rsp_size);
    }

    return 0;
}
#endif

int WeChatServer::OnPingReq(TCPSession *session, const char *data, uint32_t head_size, uint32_t body_size, uint64_t tid)
{
    PingReq req;
    if(req.ParseFromArray(data+head_size, body_size) == false)
    {
        LOG_WARN(logger, "OnPingReq:parse req failed.");
        return -1;
    }
    LOG_DEBUG(logger, "OnPingReq:session="<<session<<".req="<<req.ShortDebugString());

    //通知状态服务器(不需要回包)
    NotifyUserStatus("OnPingReq", 1, req.id(), 1);
    return 0;
}

int WeChatServer::OnRegReq(TCPSession *session, const char *data, uint32_t head_size, uint32_t body_size, uint64_t tid)
{
    RegReq req;
    if(req.ParseFromArray(data+head_size, body_size) == false)
    {
        LOG_WARN(logger, "OnRegReq:parse req failed.");
        return -1;
    }
    LOG_INFO(logger, "OnRegReq:session="<<session<<".req="<<req.ShortDebugString());

    UserInfo *user_info = SaveNewUser(req);

    RegRsp rsp;
    if(user_info == NULL)
    {
        rsp.set_ret(-1);
    }
    else
    {
        rsp.set_ret(0);
        rsp.set_id(user_info->id());
    }

    if(RspSvr(session, CMD_REQ_RSP, &rsp, 0) != 0)
    {
        LOG_WARN(logger, "OnRegReq|send failed,session="<<session<<",rsp="<<rsp.ShortDebugString());
    }
    else
    {
        LOG_DEBUG(logger, "OnRegReq|send successful,session="<<session<<",rsp="<<rsp.ShortDebugString());
    }

    return 0;
}

int WeChatServer::OnLoginReq(TCPSession *session, const char *data, uint32_t head_size, uint32_t body_size, uint64_t tid)
{
    LoginReq req;
    if(req.ParseFromArray(data+head_size, body_size) == false)
    {
        LOG_WARN(logger, "OnLoginReq:parse req failed.");
        return -1;
    }
    LOG_INFO(logger, "OnLoginReq:session="<<session<<".req="<<req.ShortDebugString());

    SessionUserInfoMap::iterator it = m_SessionUserInfoMap.find(session);
    if(it != m_SessionUserInfoMap.end())
    {
        LOG_WARN(logger, "OnLoginReq:already login.session="<<session<<".req="<<req.ShortDebugString());
        return 0;
    }

    //检查登录信息
    UserInfo *user_info = LoginCheck(req);

    //设置回包
    LoginRsp rsp;
    if(user_info == NULL)
    {
        LOG_WARN(logger, "OnLoginReq|LoginCheck failed.session="<<session<<",req="<<req.ShortDebugString());
        rsp.set_ret(-1);
    }
    else
    {
        rsp.set_ret(0);
        rsp.set_id(user_info->id());
        rsp.set_name(user_info->name());
        FillMemberList(user_info->id(), *(rsp.mutable_member_list()));

        //通知状态服务器(不需要回包)
        NotifyUserStatus("OnLoginReq", 1, user_info->id(), 1);
    }

    //回复登录回包
    if(RspSvr(session, CMD_LOGIN_RSP, &rsp, 0) != 0)
    {
        LOG_WARN(logger, "OnLoginReq|send failed,session="<<session<<",rsp="<<rsp.ShortDebugString());
    }
    else
    {
        LOG_DEBUG(logger, "OnLoginReq|send successful,session="<<session<<",rsp="<<rsp.ShortDebugString());
    }

    //保存会话
    if(user_info == NULL)
    {
        return 0;
    }
    std::pair<SessionUserInfoMap::iterator, bool> ret = m_SessionUserInfoMap.insert(std::make_pair(session, user_info));
    if(ret.second == false)
    {
        LOG_WARN(logger, "OnLoginReq:save SessionUserInfo failed.session="<<session);
        return -1;
    }

    //广播登录消息

    Msg msg;
    msg.set_type(0);
    msg.set_send_id(user_info->id());
    msg.set_send_name(user_info->name());
    msg.set_time(time(NULL));

    std::ostringstream oss;
    oss<<"\""<<user_info->name()<<"\" login...";
    msg.set_msg(oss.str());

    BroadcastMsg(msg);

    return 0;
}

int WeChatServer::OnLogoutReq(TCPSession *session, const char *data, uint32_t head_size, uint32_t body_size, uint64_t tid)
{
    LogoutReq req;
    if(req.ParseFromArray(data+head_size, body_size) == false)
    {
        LOG_WARN(logger, "OnLogoutReq:parse req failed.");
        return -1;
    }
    LOG_INFO(logger, "OnLogoutReq:session="<<session<<".req="<<req.ShortDebugString());
    UserInfoMap::iterator it = m_UserInfoMap.find(req.id());
    if(it == m_UserInfoMap.end())
    {
        return -1;
    }

    Msg msg;
    msg.set_type(1);
    msg.set_send_id(it->second.id());
    msg.set_send_name(it->second.name());
    msg.set_time(time(NULL));

    std::ostringstream oss;
    oss.clear();
    oss<<"\""<<it->second.name()<<"\" logout...";
    msg.set_msg(oss.str());

    m_SessionUserInfoMap.erase(session);
    BroadcastMsg(msg);

    return 0;
}

int WeChatServer::OnSendMsgReq(TCPSession *session, const char *data, uint32_t head_size, uint32_t body_size, uint64_t tid)
{
    SendMsgReq req;
    if(req.ParseFromArray(data+head_size, body_size) == false)
    {
        LOG_WARN(logger, "OnSendMsgReq:parse req failed.");
        return -1;
    }
    LOG_INFO(logger, "OnSendMsgReq:session="<<session<<".req="<<req.ShortDebugString());

    SessionUserInfoMap::iterator it = m_SessionUserInfoMap.find(session);
    if(it == m_SessionUserInfoMap.end())
    {
        LOG_WARN(logger, "OnSendMsgReq:can't find session.session="<<session<<".req="<<req.ShortDebugString());
        return -2;
    }
    const UserInfo &user_info = *(it->second);

    Msg msg;
    msg.set_type(2);
    msg.set_send_id(user_info.id());
    msg.set_send_name(user_info.name());
    msg.set_msg(req.msg());
    msg.set_time(time(NULL));

    BroadcastMsg(msg);
    return 0;
}

void WeChatServer::BroadcastMsg(Msg &msg)
{
    SessionUserInfoMap::iterator it;
    for(it=m_SessionUserInfoMap.begin(); it!=m_SessionUserInfoMap.end(); ++it)
    {
        if(RspSvr(it->first, CMD_RECV_MSG_REQ, &msg, 0) != 0)
        {
            LOG_WARN(logger, "BroadcastMsg:send failed,session="<<it->first<<",msg="<<msg.ShortDebugString());
        }
        else
        {
            LOG_DEBUG(logger, "BroadcastMsg:send successful,session="<<it->first<<",msg="<<msg.ShortDebugString());
        }
    }
}

UserInfo* WeChatServer::SaveNewUser(RegReq &req)
{
    string user_count;
    string USER_COUNT_KEY = "USER_COUNT";

    uint32_t id = 10000;
    if(m_DBWrap->Get(USER_COUNT_KEY, user_count) == true)
    {
        id = *(uint32_t*)(user_count.data()) + 1;
    }

    UserInfo user_info;
    user_info.set_id(id);
    user_info.set_name(req.name());
    user_info.set_passwd(req.passwd());

    //保存注册信息到db
    string value;
    user_info.SerializeToString(&value);
    if(m_DBWrap->Put(user_info.id(), value) == false)
    {
        LOG_WARN(logger, "SaveNewUser to db failed. req="<<req.ShortDebugString());
        return NULL;
    }
    //保存计数到db
    value.assign((char*)&id, sizeof(id));
    if(m_DBWrap->Put(USER_COUNT_KEY, value) == false)
    {
        LOG_WARN(logger, "SaveNewUser count to db failed. req="<<req.ShortDebugString());
        return NULL;
    }

    std::pair<UserInfoMap::iterator, bool> ret = m_UserInfoMap.insert(std::make_pair(user_info.id(), user_info));
    return ret.second == false?NULL:&(ret.first->second);
}

UserInfo* WeChatServer::LoginCheck(LoginReq &req)
{
    UserInfoMap::iterator it = m_UserInfoMap.find(req.id());
    if(it == m_UserInfoMap.end())
    {
        //从db里面找以下
        string value;
        if(m_DBWrap->Get(req.id(), value) == false)  //db里面也没有(没有注册过)
        {
            return NULL;
        }

        //从db拉回来的用户信息放到内存里面
        UserInfo user_info;
        user_info.ParseFromString(value);
        std::pair<UserInfoMap::iterator, bool> ret = m_UserInfoMap.insert(std::make_pair(user_info.id(), user_info));
        it = ret.first;
    }

    UserInfo *user_info = &(it->second);
    if(user_info->passwd() != req.passwd())
    {
        return NULL;
    }

    return user_info;
}

void WeChatServer::FillMemberList(uint32_t id, MemberList &member_list)
{
    UserInfoMap::iterator it;
    for(it=m_UserInfoMap.begin();it!=m_UserInfoMap.end(); ++it)
    {
        const UserInfo &user_info = it->second;
        MemberList::Member &member = *member_list.add_member();
        member.set_id(user_info.id());
        member.set_name(user_info.name());
    }
}

void WeChatServer::NotifyUserStatus(const string &log_flag, uint32_t type, uint32_t id, uint32_t access_index)
{
    //通知状态服务器(不需要回包)
    NotifyUserStatusReq status_req;
    status_req.set_type(type);
    UserStatus * user_status = status_req.mutable_user_status();
    user_status->set_uid(id);
    user_status->set_access_index(access_index);
    int ret = ReqSvr(CMD_NOTIFY_USERSTATUS_REQ, &status_req, 0, NULL);
    if(ret != 0)
    {
        LOG_WARN(logger, "NotifyUserStatus:"<<log_flag<<".send NotifyUserStatus failed.ret="<<ret<<",req="<<status_req.ShortDebugString());
    }
    else
    {
        LOG_DEBUG(logger, "NotifyUserStatus:"<<log_flag<<".send NotifyUserStatus succ.ret="<<ret<<",req="<<status_req.ShortDebugString());
    }
}
