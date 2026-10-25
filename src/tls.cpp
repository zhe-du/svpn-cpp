// TLS 客户端（SChannel）+ ClientHello SNI 解析。
//
// 对齐 Python 基准：svpn/netutil.py 的 tls_over() / tls_probe()、svpn/forward.py 的 extract_sni()。
//
// 设计要点：
//   * 手工验证证书（SCH_CRED_MANUAL_CRED_VALIDATION + ISC_REQ_MANUAL_CRED_VALIDATION），
//     系统绝不弹信任框；验证结论只记进状态，由 tls_probe 决定是否算失败。
//   * SNI 走 InitializeSecurityContextW 的 pszTargetName（UTF-16 域名），SChannel 自动生成扩展。
//   * 握手期多读的字节（同一 TCP 段里跟在 ServerHello 后面的记录）必须缓存给 DecryptMessage，
//     丢了就会解密失败——这是最经典的坑。
//   * close() 幂等：ctx 与 sock 各自只释放一次。
#include "svpn.hpp"

#define SECURITY_WIN32 1   // security.h/sspi.h 要求先选平台
#include <winsock2.h>      // 必须先于 windows.h
#include <windows.h>

#include <schannel.h>
#include <security.h>
#include <wincrypt.h>

#include <algorithm>
#include <cstring>
#include <vector>

namespace svpn {
namespace {

// ---------------------------------------------------------------- 内部状态
// TlsConn.ctx 指向它（堆上，close() 里 delete）。
struct TlsState {
    CredHandle cred{};
    CtxtHandle ctx{};
    bool have_cred{false};
    bool have_ctx{false};
    bool close_notify{false};      // 已发过 close_notify / 已放弃发送

    SecPkgContext_StreamSizes sizes{};

    std::vector<unsigned char> ct;      // 待解密的密文（握手残留 + 半条记录）
    std::vector<unsigned char> pt;      // 已解密、还没交出去的明文
    bool eof{false};

    int timeout_ms{kDefaultTimeoutMs};

    std::string cert_cn;                // 对端证书 CN（UTF-8）
    std::string protocol;               // TLS1.2 / TLS1.3 / ...
    std::string cert_err;               // 证书校验失败原因（空 = 通过）
};

constexpr DWORD kIscFlags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT |
                            ISC_REQ_CONFIDENTIALITY | ISC_REQ_EXTENDED_ERROR |
                            ISC_REQ_ALLOCATE_MEMORY | ISC_REQ_STREAM |
                            ISC_REQ_MANUAL_CRED_VALIDATION;

// ---------------------------------------------------------------- 编解码
std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int need = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out(static_cast<std::size_t>(need), L'\0');
    int got = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), out.data(), need);
    if (got <= 0) return std::wstring();
    out.resize(static_cast<std::size_t>(got));
    return out;
}

std::string wide_to_utf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    int need = ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                     nullptr, 0, nullptr, nullptr);
    if (need <= 0) return std::string();
    std::string out(static_cast<std::size_t>(need), '\0');
    int got = ::WideCharToMultiByte(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()),
                                    out.data(), need, nullptr, nullptr);
    if (got <= 0) return std::string();
    out.resize(static_cast<std::size_t>(got));
    return out;
}

// Winsock 错误 → 中文短语（文案口径对齐 Python 的 ConnectionResetError / timeout 分类）
std::string wsa_text(int code) {
    switch (code) {
        case WSAETIMEDOUT: return "连接超时";
        case WSAECONNRESET: return "连接被重置(RST)";
        case WSAECONNREFUSED: return "连接被拒绝";
        case WSAENETUNREACH: return "网络不可达";
        case WSAEHOSTUNREACH: return "主机不可达";
        case WSAECONNABORTED: return "连接中断";
        case WSAENOTCONN: return "连接已关闭";
        case WSAENOBUFS: return "缓冲区不足";
        default: return fmt("网络错误(WSA %d)", code);
    }
}

// SSPI / SChannel 状态码 → 中文短语
std::string sspi_text(SECURITY_STATUS ss) {
    switch (static_cast<long>(ss)) {
        case SEC_E_WRONG_PRINCIPAL: return "证书主机名不匹配";
        case SEC_E_UNTRUSTED_ROOT: return "根证书不受信任";
        case SEC_E_CERT_EXPIRED: return "证书已过期";
        case SEC_E_CERT_UNKNOWN: return "证书无效";
        case SEC_E_ILLEGAL_MESSAGE: return "握手报文非法(可能被重置/阻断)";
        case SEC_E_ALGORITHM_MISMATCH: return "协议/算法协商失败";
        case SEC_E_INVALID_TOKEN: return "握手令牌非法";
        case SEC_E_INCOMPLETE_CREDENTIALS: return "需要客户端证书";
        case SEC_E_UNSUPPORTED_FUNCTION: return "系统不支持该 TLS 功能";
        case SEC_E_NO_CREDENTIALS: return "凭据获取失败";
        case SEC_E_MESSAGE_ALTERED: return "报文被篡改";
        case SEC_E_TARGET_UNKNOWN: return "目标不明";
        case SEC_E_INTERNAL_ERROR: return "SChannel 内部错误";
        case SEC_E_NO_AUTHENTICATING_AUTHORITY: return "找不到认证机构";
        case SEC_E_LOGON_DENIED: return "登录被拒";
        case SEC_E_BUFFER_TOO_SMALL: return "缓冲区过小";
        case SEC_E_DECRYPT_FAILURE: return "解密失败";
        case SEC_E_ENCRYPT_FAILURE: return "加密失败";
        default: return fmt("TLS 握手失败(0x%08lX)", static_cast<unsigned long>(ss));
    }
}

// 证书链策略错误 → 中文短语
std::string chain_err_text(LONG e) {
    switch (static_cast<long>(e)) {
        case CERT_E_CN_NO_MATCH: return "证书主机名不匹配";
        case CERT_E_UNTRUSTEDROOT: return "根证书不受信任";
        case CERT_E_CHAINING: return "证书链不完整";
        case CERT_E_EXPIRED: return "证书已过期";
        case CERT_E_VALIDITYPERIODNESTING: return "证书有效期异常";
        case CERT_E_REVOKED: return "证书已被吊销";
        case CERT_E_REVOCATION_FAILURE: return "吊销状态查询失败";
        case CERT_E_WRONG_USAGE: return "证书用途不符";
        case CERT_E_PURPOSE: return "证书用途不符";
        case CERT_E_ROLE: return "证书角色不符";
        case TRUST_E_CERT_SIGNATURE: return "证书签名无效";
        case CRYPT_E_NO_REVOCATION_CHECK: return "无法检查吊销状态";
        case CRYPT_E_REVOCATION_OFFLINE: return "吊销服务器离线";
        default: return fmt("证书校验失败(0x%08lX)", static_cast<unsigned long>(e));
    }
}

std::string protocol_text(DWORD p) {
    switch (p) {
        case SP_PROT_TLS1_3_CLIENT: return "TLS1.3";
        case SP_PROT_TLS1_2_CLIENT: return "TLS1.2";
        case SP_PROT_TLS1_1_CLIENT: return "TLS1.1";
        case SP_PROT_TLS1_CLIENT: return "TLS1.0";
        case SP_PROT_SSL3_CLIENT: return "SSL3.0";
        default: return std::string();
    }
}

// ---------------------------------------------------------------- 收发原语
bool read_more(socket_t s, std::vector<unsigned char>& v, int timeout_ms) {
    unsigned char tmp[16384];
    int r = recv_some(s, tmp, sizeof(tmp), timeout_ms);
    if (r <= 0) return false;
    v.insert(v.end(), tmp, tmp + r);
    return true;
}

// ---------------------------------------------------------------- 证书
void read_peer_cert(TlsState* st, const std::string& server_name) {
    PCCERT_CONTEXT cert = nullptr;
    SECURITY_STATUS dbg = QueryContextAttributesW(&st->ctx, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &cert);
    if (dbg != SEC_E_OK || !cert) {
        st->cert_err = "无法读取对端证书";
        return;
    }

    wchar_t cn[512] = {0};
    DWORD got = CertGetNameStringW(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, cn,
                                   static_cast<DWORD>(std::size(cn)));
    if (got > 1) st->cert_cn = wide_to_utf8(std::wstring(cn, got - 1));

    // 建链 + 策略校验：链建不起来只记原因，不算致命（探测场景下仍要能拿到 CN/协议）
    CERT_CHAIN_PARA para{};
    para.cbSize = sizeof(para);
    PCCERT_CHAIN_CONTEXT chain = nullptr;
    if (!CertGetCertificateChain(nullptr, cert, nullptr, cert->hCertStore, &para, 0, nullptr, &chain) ||
        !chain) {
        st->cert_err = chain_err_text(static_cast<LONG>(GetLastError()));
        CertFreeCertificateContext(cert);
        return;
    }

    std::wstring wname = utf8_to_wide(server_name);
    SSL_EXTRA_CERT_CHAIN_POLICY_PARA sslp{};
    sslp.cbSize = sizeof(sslp);
    sslp.dwAuthType = AUTHTYPE_SERVER;
    sslp.fdwChecks = 0;
    sslp.pwszServerName = wname.empty() ? nullptr : const_cast<wchar_t*>(wname.c_str());

    CERT_CHAIN_POLICY_PARA pol{};
    pol.cbSize = sizeof(pol);
    pol.dwFlags = 0;
    pol.pvExtraPolicyPara = &sslp;

    CERT_CHAIN_POLICY_STATUS stat{};
    stat.cbSize = sizeof(stat);

    if (!CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &pol, &stat)) {
        st->cert_err = chain_err_text(static_cast<LONG>(GetLastError()));
    } else if (stat.dwError != 0) {
        st->cert_err = chain_err_text(static_cast<LONG>(stat.dwError));
    }

    CertFreeCertificateChain(chain);
    CertFreeCertificateContext(cert);
}

// ---------------------------------------------------------------- 握手
// data：调用前已经收到的字节（首次为空；重协商时是 last 的 SECBUFFER_EXTRA）。
// 成功后 st->ct 保存「握手记录之后」的残留密文。
bool do_handshake(TlsState* st, socket_t sock, const std::wstring& target,
                  std::vector<unsigned char> data, std::string* err) {
    for (int round = 0; round < 64; ++round) {
        SecBuffer out{};
        out.BufferType = SECBUFFER_TOKEN;
        out.pvBuffer = nullptr;
        out.cbBuffer = 0;
        SecBufferDesc outd{};
        outd.ulVersion = SECBUFFER_VERSION;
        outd.cBuffers = 1;
        outd.pBuffers = &out;

        SecBuffer in[2]{};
        SecBufferDesc ind{};
        ind.ulVersion = SECBUFFER_VERSION;
        ind.cBuffers = 0;
        ind.pBuffers = in;
        PSecBufferDesc pind = nullptr;
        if (!data.empty()) {
            in[0].BufferType = SECBUFFER_TOKEN;
            in[0].pvBuffer = data.data();
            in[0].cbBuffer = static_cast<unsigned long>(data.size());
            in[1].BufferType = SECBUFFER_EMPTY;
            in[1].pvBuffer = nullptr;
            in[1].cbBuffer = 0;
            ind.cBuffers = 2;
            pind = &ind;
        }

        DWORD attrs = 0;
        SECURITY_STATUS ss = ::InitializeSecurityContextW(
            &st->cred, st->have_ctx ? &st->ctx : nullptr,
            target.empty() ? nullptr : const_cast<SEC_WCHAR*>(target.c_str()), kIscFlags, 0,
            SECURITY_NATIVE_DREP, pind, 0, &st->ctx, &outd, &attrs, nullptr);

        if (st->have_ctx == false) st->have_ctx = true;   // 首轮返回即已建上下文

        if (ss == SEC_I_COMPLETE_NEEDED || ss == SEC_I_COMPLETE_AND_CONTINUE) {
            if (::CompleteAuthToken(&st->ctx, &outd) != SEC_E_OK) {
                if (out.pvBuffer) FreeContextBuffer(out.pvBuffer);
                if (err) *err = "完成握手令牌失败";
                return false;
            }
            ss = (ss == SEC_I_COMPLETE_NEEDED) ? SEC_E_OK : SEC_I_CONTINUE_NEEDED;
        }

        if (out.cbBuffer != 0 && out.pvBuffer != nullptr) {
            bool sent = send_all(sock, out.pvBuffer, out.cbBuffer, st->timeout_ms);
            FreeContextBuffer(out.pvBuffer);
            if (!sent) {
                if (err) *err = wsa_text(WSAGetLastError());
                return false;
            }
        }

        // 本轮没被吃掉的尾巴
        std::vector<unsigned char> extra;
        if (pind != nullptr && in[1].BufferType == SECBUFFER_EXTRA && in[1].cbBuffer <= data.size()) {
            std::size_t keep = in[1].cbBuffer;
            extra.assign(data.begin() + static_cast<std::ptrdiff_t>(data.size() - keep), data.end());
        }

        if (ss == SEC_E_OK) {
            st->ct = std::move(extra);
            return true;
        }
        if (ss == SEC_E_INCOMPLETE_MESSAGE) {
            // 输入整体没被消费，data 原样保留，继续读
        } else if (ss == SEC_I_CONTINUE_NEEDED || ss == SEC_I_INCOMPLETE_CREDENTIALS) {
            data = std::move(extra);
        } else {
            if (err) *err = sspi_text(ss);
            return false;
        }

        if (!read_more(sock, data, st->timeout_ms)) {
            if (err) *err = wsa_text(WSAGetLastError());
            return false;
        }
    }
    if (err) *err = "握手轮次过多（对端行为异常）";
    return false;
}

void send_close_notify(TlsConn* c, TlsState* st) {
    if (c->sock == kBadSock || !st->have_ctx) return;

    SecBuffer out{};
    out.BufferType = SECBUFFER_TOKEN;
    out.pvBuffer = nullptr;
    out.cbBuffer = 0;
    SecBufferDesc outd{};
    outd.ulVersion = SECBUFFER_VERSION;
    outd.cBuffers = 1;
    outd.pBuffers = &out;

    if (::ApplyControlToken(&st->ctx, &outd) != SEC_E_OK) return;

    SecBuffer tok{};
    tok.BufferType = SECBUFFER_TOKEN;
    tok.pvBuffer = nullptr;
    tok.cbBuffer = 0;
    SecBufferDesc tokd{};
    tokd.ulVersion = SECBUFFER_VERSION;
    tokd.cBuffers = 1;
    tokd.pBuffers = &tok;

    DWORD attrs = 0;
    SECURITY_STATUS ss = ::InitializeSecurityContextW(
        &st->cred, &st->ctx, nullptr, kIscFlags, 0, SECURITY_NATIVE_DREP, nullptr, 0, &st->ctx,
        &tokd, &attrs, nullptr);
    if ((ss == SEC_E_OK || ss == SEC_I_CONTINUE_NEEDED) && tok.cbBuffer != 0 && tok.pvBuffer) {
        send_all(c->sock, tok.pvBuffer, tok.cbBuffer, st->timeout_ms);
    }
    if (tok.pvBuffer) FreeContextBuffer(tok.pvBuffer);
}

}  // namespace

// ================================================================ TlsConn
int TlsConn::send(const void* data, std::size_t n) {
    if (!ok || ctx == nullptr || sock == kBadSock) return -1;
    if (n == 0) return 0;
    auto* st = static_cast<TlsState*>(ctx);
    if (st->eof) return -1;

    const unsigned char* src = static_cast<const unsigned char*>(data);
    std::size_t done = 0;
    while (done < n) {
        std::size_t chunk = std::min<std::size_t>(n - done, st->sizes.cbMaximumMessage);
        if (chunk == 0) return -1;

        std::size_t total = st->sizes.cbHeader + chunk + st->sizes.cbTrailer;
        std::vector<unsigned char> rec(total);

        SecBuffer sb[4]{};
        sb[0].BufferType = SECBUFFER_STREAM_HEADER;
        sb[0].pvBuffer = rec.data();
        sb[0].cbBuffer = st->sizes.cbHeader;
        sb[1].BufferType = SECBUFFER_DATA;
        sb[1].pvBuffer = rec.data() + st->sizes.cbHeader;
        sb[1].cbBuffer = static_cast<unsigned long>(chunk);
        std::memcpy(sb[1].pvBuffer, src + done, chunk);
        sb[2].BufferType = SECBUFFER_STREAM_TRAILER;
        sb[2].pvBuffer = rec.data() + st->sizes.cbHeader + chunk;
        sb[2].cbBuffer = st->sizes.cbTrailer;
        sb[3].BufferType = SECBUFFER_EMPTY;
        sb[3].pvBuffer = nullptr;
        sb[3].cbBuffer = 0;

        SecBufferDesc sd{};
        sd.ulVersion = SECBUFFER_VERSION;
        sd.cBuffers = 4;
        sd.pBuffers = sb;

        SECURITY_STATUS ss = ::EncryptMessage(&st->ctx, 0, &sd, 0);
        if (ss != SEC_E_OK) {
            log_write("WARN", fmt("[tls] 加密失败: %s", sspi_text(ss).c_str()));
            return -1;
        }

        std::size_t used = sb[0].cbBuffer + sb[1].cbBuffer + sb[2].cbBuffer;
        if (used == 0 || used > total) return -1;
        if (!send_all(sock, rec.data(), used, st->timeout_ms)) return -1;
        done += chunk;
    }
    return static_cast<int>(done);
}

int TlsConn::recv(void* buf, std::size_t cap) {
    if (!ok || ctx == nullptr || sock == kBadSock) return -1;
    if (cap == 0) return 0;
    auto* st = static_cast<TlsState*>(ctx);
    if (!st->pt.empty()) {
        std::size_t take = std::min(cap, st->pt.size());
        std::memcpy(buf, st->pt.data(), take);
        st->pt.erase(st->pt.begin(), st->pt.begin() + static_cast<std::ptrdiff_t>(take));
        return static_cast<int>(take);
    }
    if (st->eof) return 0;

    for (int guard = 0; guard < 256; ++guard) {
        // 记录头 5 字节都不够就没法解，先读
        if (st->ct.size() >= 5) {
            SecBuffer sb[4]{};
            sb[0].BufferType = SECBUFFER_DATA;
            sb[0].pvBuffer = st->ct.data();
            sb[0].cbBuffer = static_cast<unsigned long>(st->ct.size());
            sb[1].BufferType = SECBUFFER_EMPTY;
            sb[2].BufferType = SECBUFFER_EMPTY;
            sb[3].BufferType = SECBUFFER_EMPTY;
            sb[1].pvBuffer = sb[2].pvBuffer = sb[3].pvBuffer = nullptr;
            sb[1].cbBuffer = sb[2].cbBuffer = sb[3].cbBuffer = 0;

            SecBufferDesc sd{};
            sd.ulVersion = SECBUFFER_VERSION;
            sd.cBuffers = 4;
            sd.pBuffers = sb;

            SECURITY_STATUS ss = ::DecryptMessage(&st->ctx, &sd, 0, nullptr);

            if (ss == SEC_E_OK) {
                for (int i = 0; i < 4; ++i) {
                    if (sb[i].BufferType == SECBUFFER_DATA && sb[i].cbBuffer > 0 && sb[i].pvBuffer) {
                        const unsigned char* p = static_cast<const unsigned char*>(sb[i].pvBuffer);
                        st->pt.insert(st->pt.end(), p, p + sb[i].cbBuffer);
                    }
                }
                // 多余字节搬到 ct 头部
                for (int i = 0; i < 4; ++i) {
                    if (sb[i].BufferType == SECBUFFER_EXTRA && sb[i].cbBuffer > 0) {
                        std::size_t keep = sb[i].cbBuffer;
                        if (keep > st->ct.size()) keep = st->ct.size();
                        std::vector<unsigned char> tail(
                            st->ct.end() - static_cast<std::ptrdiff_t>(keep), st->ct.end());
                        st->ct.swap(tail);
                        break;
                    }
                }
                if (!st->pt.empty()) {
                    std::size_t take = std::min(cap, st->pt.size());
                    std::memcpy(buf, st->pt.data(), take);
                    st->pt.erase(st->pt.begin(), st->pt.begin() + static_cast<std::ptrdiff_t>(take));
                    return static_cast<int>(take);
                }
                continue;   // 只有控制记录，继续解
            }

            if (ss == SEC_I_CONTEXT_EXPIRED) {   // 对端 close_notify
                st->eof = true;
                return 0;
            }
            if (ss == SEC_E_INCOMPLETE_MESSAGE) {
                // 继续读
            } else if (ss == SEC_I_RENEGOTIATE) {
                std::vector<unsigned char> extra;
                for (int i = 0; i < 4; ++i) {
                    if (sb[i].BufferType == SECBUFFER_EXTRA && sb[i].cbBuffer > 0) {
                        std::size_t keep = sb[i].cbBuffer;
                        if (keep > st->ct.size()) keep = st->ct.size();
                        extra.assign(st->ct.end() - static_cast<std::ptrdiff_t>(keep), st->ct.end());
                        break;
                    }
                }
                st->ct.clear();
                std::string err;
                if (!do_handshake(st, sock, std::wstring(), std::move(extra), &err)) {
                    log_write("WARN", fmt("[tls] 重协商失败: %s", err.c_str()));
                    st->eof = true;
                    return -1;
                }
                continue;
            } else {
                // 注意这里是 else：SEC_E_INCOMPLETE_MESSAGE 属于「还没收全」，
                // 必须落到下面的 read_more 继续读，绝不能当失败返回 -1。
                // （老代码少了这个 else，"解密失败: 0x80090318" 会直接在第一个
                //   应用数据记录上把连接掐掉 —— 握手全对、一收正文就断。）
                log_write("WARN", fmt("[tls] 解密失败: %s", sspi_text(ss).c_str()));
                st->eof = true;
                return -1;
            }
        }

        if (!read_more(sock, st->ct, st->timeout_ms)) {
            st->eof = true;
            return 0;   // 对端直接关 TCP：与 Python 的 recv() == b"" 等价
        }
    }
    return -1;
}

void TlsConn::close() {
    auto* st = static_cast<TlsState*>(ctx);
    if (st != nullptr) {
        if (!st->close_notify && ok) {
            st->close_notify = true;
            send_close_notify(this, st);
        }
        if (st->have_ctx) {
            ::DeleteSecurityContext(&st->ctx);
            st->have_ctx = false;
        }
        if (st->have_cred) {
            ::FreeCredentialsHandle(&st->cred);
            st->have_cred = false;
        }
        delete st;
        ctx = nullptr;
    }
    ok = false;
    if (sock != kBadSock) {
        close_socket(sock);
        sock = kBadSock;
    }
}

// ================================================================ 握手入口
TlsConn tls_connect(socket_t sock, const std::string& server_name, int timeout_ms,
                    std::string* err) {
    TlsConn c;
    c.sock = sock;

    if (sock == kBadSock) {
        if (err) *err = "无效的连接";
        return c;
    }
    if (timeout_ms <= 0) timeout_ms = kDefaultTimeoutMs;

    auto* st = new TlsState();
    st->timeout_ms = timeout_ms;
    c.ctx = st;

    set_io_timeout(sock, timeout_ms);

    SCHANNEL_CRED sc{};
    sc.dwVersion = SCHANNEL_CRED_VERSION;
    sc.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT | SP_PROT_TLS1_3_CLIENT;
    sc.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION | SCH_CRED_NO_DEFAULT_CREDS |
                 SCH_CRED_NO_SERVERNAME_CHECK | SCH_USE_STRONG_CRYPTO;
    sc.dwMinimumCipherStrength = 128;

    SECURITY_STATUS ss = ::AcquireCredentialsHandleW(
        nullptr, const_cast<SEC_WCHAR*>(UNISP_NAME_W), SECPKG_CRED_OUTBOUND, nullptr, &sc,
        nullptr, nullptr, &st->cred, nullptr);
    if (ss != SEC_E_OK) {
        if (err) *err = sspi_text(ss);
        return c;   // sock 仍是 c.sock，调用方 close() 会关
    }
    st->have_cred = true;

    std::wstring target = utf8_to_wide(server_name);
    std::vector<unsigned char> none;
    if (!do_handshake(st, sock, target, std::move(none), err)) {
        return c;
    }

    ss = ::QueryContextAttributesW(&st->ctx, SECPKG_ATTR_STREAM_SIZES, &st->sizes);
    if (ss != SEC_E_OK) {
        if (err) *err = sspi_text(ss);
        return c;
    }

    SecPkgContext_ConnectionInfo info{};
    if (::QueryContextAttributesW(&st->ctx, SECPKG_ATTR_CONNECTION_INFO, &info) == SEC_E_OK) {
        st->protocol = protocol_text(info.dwProtocol);
    }

    read_peer_cert(st, server_name);

    c.ok = true;
    return c;
}

// ================================================================ 一次性探测
TlsProbe tls_probe(const std::string& ip, const std::string& sni, int timeout_ms) {
    TlsProbe r;
    if (timeout_ms <= 0) timeout_ms = kDefaultTimeoutMs;
    std::int64_t t0 = now_ms();

    socket_t s = tcp_connect_ip(ip, 443, timeout_ms);
    if (s == kBadSock) {
        double spent = static_cast<double>(now_ms() - t0);
        r.ok = false;
        r.err = (spent >= static_cast<double>(timeout_ms) - 5.0) ? "连接超时"
                                                                : "TCP 连接失败(不可达或被拒)";
        r.ms = spent;
        return r;
    }

    std::string err;
    TlsConn c = tls_connect(s, sni, timeout_ms, &err);
    if (!c.ok || c.ctx == nullptr) {
        c.close();
        r.ok = false;
        r.err = err.empty() ? "TLS 握手失败" : err;
        r.ms = static_cast<double>(now_ms() - t0);
        return r;
    }

    auto* st = static_cast<TlsState*>(c.ctx);
    r.cert_cn = st->cert_cn;
    r.protocol = st->protocol;
    if (!st->cert_err.empty()) {
        r.ok = false;
        r.err = st->cert_err + "（SNI=" + sni + "）";
    } else {
        r.ok = true;
    }
    c.close();
    r.ms = static_cast<double>(now_ms() - t0);
    return r;
}

// ================================================================ ClientHello SNI
// 纯字节解析，任何越界一律 false（输入来自任意字节流）。
bool parse_client_hello_sni(const unsigned char* buf, std::size_t n, std::string* sni) {
    if (buf == nullptr || n < 5) return false;
    if (buf[0] != 0x16) return false;   // handshake
    if (buf[1] != 0x03) return false;   // TLS

    std::size_t rec_len = (static_cast<std::size_t>(buf[3]) << 8) | buf[4];
    if (rec_len < 4) return false;
    if (n < 5 + rec_len) return false;   // 分片：调用方继续读

    const unsigned char* hs = buf + 5;
    std::size_t hn = rec_len;
    if (hs[0] != 0x01) return false;    // client_hello

    std::size_t body = (static_cast<std::size_t>(hs[1]) << 16) |
                       (static_cast<std::size_t>(hs[2]) << 8) | hs[3];
    if (body + 4 > hn) return false;    // 握手体没到齐

    // 游标式解析：每一步都先检查剩余长度
    std::size_t p = 4;
    const std::size_t end = 4 + body;

    auto need = [&](std::size_t k) { return p + k <= end; };

    if (!need(2 + 32)) return false;    // legacy_version + random
    p += 2 + 32;

    if (!need(1)) return false;
    std::size_t sid = hs[p];
    p += 1;
    if (!need(sid)) return false;
    p += sid;

    if (!need(2)) return false;
    std::size_t cs = (static_cast<std::size_t>(hs[p]) << 8) | hs[p + 1];
    p += 2;
    if (!need(cs)) return false;
    p += cs;

    if (!need(1)) return false;
    std::size_t comp = hs[p];
    p += 1;
    if (!need(comp)) return false;
    p += comp;

    if (!need(2)) return false;         // 没有扩展段（老客户端）
    std::size_t ext_total = (static_cast<std::size_t>(hs[p]) << 8) | hs[p + 1];
    p += 2;
    if (p + ext_total > end) return false;   // 扩展段被截断
    std::size_t ext_end = p + ext_total;

    while (p + 4 <= ext_end) {
        std::size_t type = (static_cast<std::size_t>(hs[p]) << 8) | hs[p + 1];
        std::size_t len = (static_cast<std::size_t>(hs[p + 2]) << 8) | hs[p + 3];
        p += 4;
        if (p + len > ext_end) return false;
        if (type != 0x0000) {   // server_name
            p += len;
            continue;
        }
        // server_name 扩展：list_len(2) + [ name_type(1) + name_len(2) + name ]
        if (len < 2) return false;
        std::size_t list_len = (static_cast<std::size_t>(hs[p]) << 8) | hs[p + 1];
        std::size_t q = p + 2;
        std::size_t list_end = q + list_len;
        if (list_len == 0 || list_end > p + len) return false;
        while (q + 3 <= list_end) {
            std::size_t nt = hs[q];
            std::size_t nl = (static_cast<std::size_t>(hs[q + 1]) << 8) | hs[q + 2];
            q += 3;
            if (q + nl > list_end) return false;
            if (nt != 0) {   // 0 = host_name
                q += nl;
                continue;
            }
            if (nl == 0 || nl > 253) return false;
            std::string name(reinterpret_cast<const char*>(hs + q), nl);
            for (char ch : name) {
                unsigned char u = static_cast<unsigned char>(ch);
                if (u < 0x21 || u == 0x7f) return false;   // 控制字符/空格：不是域名
            }
            for (char& ch : name) {
                if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
            }
            while (!name.empty() && name.back() == '.') name.pop_back();
            if (name.empty()) return false;
            if (sni) *sni = name;
            return true;
        }
        return false;
    }
    return false;
}

}  // namespace svpn
