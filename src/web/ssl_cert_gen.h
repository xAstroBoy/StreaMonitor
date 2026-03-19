#pragma once

// ─────────────────────────────────────────────────────────────────
// StreaMonitor C++ — Self-Signed SSL Certificate Generator
// Auto-generates a self-signed cert + key on first launch for HTTPS
// Uses OpenSSL APIs directly (already linked via vcpkg)
// ─────────────────────────────────────────────────────────────────

#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#include <openssl/rsa.h>
#include <spdlog/spdlog.h>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <wincrypt.h>
#pragma comment(lib, "crypt32.lib")
#else
#include <ifaddrs.h>
#include <arpa/inet.h>
#include <cstdlib>
#endif

namespace sm
{

    struct SslCertPaths
    {
        std::string certPath;
        std::string keyPath;
    };

    // ─────────────────────────────────────────────────────────────
    // Install a PEM certificate into the OS trusted root CA store
    // so browsers trust it without warnings.
    // Windows:  Adds to Current User "ROOT" store (no admin needed)
    // Linux:    Copies to /usr/local/share/ca-certificates/ + update-ca-certificates
    // macOS:    Uses 'security add-trusted-cert' to user login keychain
    // ─────────────────────────────────────────────────────────────
    inline bool installCertToTrustStore(const std::string &certPemPath)
    {
        namespace fs = std::filesystem;

        if (!fs::exists(certPemPath))
        {
            spdlog::error("Cannot install cert: {} not found", certPemPath);
            return false;
        }

#ifdef _WIN32
        // ── Windows: Use Win32 Crypto API to add cert to Current User Root store ──
        // Read the PEM file
        FILE *fp = fopen(certPemPath.c_str(), "rb");
        if (!fp)
        {
            spdlog::error("Failed to open cert file for trust store install: {}", certPemPath);
            return false;
        }
        X509 *x509 = PEM_read_X509(fp, nullptr, nullptr, nullptr);
        fclose(fp);
        if (!x509)
        {
            spdlog::error("Failed to parse PEM certificate for trust store install");
            return false;
        }

        // Convert X509 to DER (binary) format for Windows Crypto API
        int derLen = i2d_X509(x509, nullptr);
        if (derLen <= 0)
        {
            X509_free(x509);
            spdlog::error("Failed to get DER length of certificate");
            return false;
        }
        std::vector<unsigned char> derBuf(derLen);
        unsigned char *derPtr = derBuf.data();
        i2d_X509(x509, &derPtr);
        X509_free(x509);

        // Open the Current User "Root" (Trusted Root CAs) certificate store
        HCERTSTORE hStore = CertOpenStore(
            CERT_STORE_PROV_SYSTEM,
            0,
            0,
            CERT_SYSTEM_STORE_CURRENT_USER,
            L"ROOT");
        if (!hStore)
        {
            spdlog::error("Failed to open Windows certificate store (error: {})", GetLastError());
            return false;
        }

        // Check if our cert is already installed by looking for it
        CRYPT_DATA_BLOB certBlob;
        certBlob.pbData = derBuf.data();
        certBlob.cbData = static_cast<DWORD>(derBuf.size());

        PCCERT_CONTEXT pExisting = CertFindCertificateInStore(
            hStore,
            X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
            0,
            CERT_FIND_EXISTING,
            CertCreateCertificateContext(X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
                                         derBuf.data(), static_cast<DWORD>(derBuf.size())),
            nullptr);

        if (pExisting)
        {
            CertFreeCertificateContext(pExisting);
            CertCloseStore(hStore, 0);
            spdlog::info("SSL certificate already installed in Windows trust store");
            return true;
        }

        // Add the certificate to the store
        BOOL ok = CertAddEncodedCertificateToStore(
            hStore,
            X509_ASN_ENCODING | PKCS_7_ASN_ENCODING,
            derBuf.data(),
            static_cast<DWORD>(derBuf.size()),
            CERT_STORE_ADD_REPLACE_EXISTING,
            nullptr);

        CertCloseStore(hStore, 0);

        if (ok)
        {
            spdlog::info("SSL certificate installed to Windows Trusted Root CA store (Current User)");
            return true;
        }
        else
        {
            spdlog::warn("Failed to install cert to Windows trust store (error: {}). "
                         "You may see browser SSL warnings.",
                         GetLastError());
            return false;
        }

#elif defined(__APPLE__)
        // ── macOS: Add to user login keychain ──
        std::string cmd = "security add-trusted-cert -r trustRoot -k "
                          "~/Library/Keychains/login.keychain-db \"" +
                          certPemPath + "\" 2>/dev/null";
        int ret = std::system(cmd.c_str());
        if (ret == 0)
        {
            spdlog::info("SSL certificate installed to macOS login keychain");
            return true;
        }
        else
        {
            // Try without -db suffix (older macOS)
            cmd = "security add-trusted-cert -r trustRoot -k "
                  "~/Library/Keychains/login.keychain \"" +
                  certPemPath + "\" 2>/dev/null";
            ret = std::system(cmd.c_str());
            if (ret == 0)
            {
                spdlog::info("SSL certificate installed to macOS login keychain");
                return true;
            }
            spdlog::warn("Failed to install cert to macOS keychain. You may see browser SSL warnings.");
            return false;
        }

#else
        // ── Linux: Copy to system CA dir and update ──
        const std::string caDir = "/usr/local/share/ca-certificates";
        const std::string dest = caDir + "/streamonitor-local.crt";
        // Need root for this — try with sudo
        std::string cmd = "sudo cp \"" + certPemPath + "\" \"" + dest +
                          "\" && sudo update-ca-certificates 2>/dev/null";
        int ret = std::system(cmd.c_str());
        if (ret == 0)
        {
            spdlog::info("SSL certificate installed to Linux system CA store");
            return true;
        }
        else
        {
            // Try without sudo (might already have permissions or running as root)
            cmd = "cp \"" + certPemPath + "\" \"" + dest +
                  "\" && update-ca-certificates 2>/dev/null";
            ret = std::system(cmd.c_str());
            if (ret == 0)
            {
                spdlog::info("SSL certificate installed to Linux system CA store");
                return true;
            }
            spdlog::warn("Failed to install cert to Linux CA store. You may see browser SSL warnings.");
            return false;
        }
#endif
    }

    // Generate a self-signed certificate and private key if they don't exist.
    // Returns paths to the cert and key PEM files.
    inline SslCertPaths ensureSslCert(const std::string &certDir = ".")
    {
        namespace fs = std::filesystem;

        SslCertPaths paths;
        paths.certPath = (fs::path(certDir) / "sm_cert.pem").string();
        paths.keyPath = (fs::path(certDir) / "sm_key.pem").string();

        // If both files exist, reuse them
        if (fs::exists(paths.certPath) && fs::exists(paths.keyPath))
        {
            spdlog::info("Using existing SSL certificate: {}", paths.certPath);
            // Ensure cert is installed in system trust store every time
            installCertToTrustStore(paths.certPath);
            return paths;
        }

        spdlog::info("Generating self-signed SSL certificate...");

        // Generate RSA key pair using EVP API (OpenSSL 3.x compatible)
        EVP_PKEY *pkey = EVP_PKEY_new();
        if (!pkey)
        {
            spdlog::error("Failed to allocate EVP_PKEY");
            return {};
        }

        EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
        if (!ctx)
        {
            EVP_PKEY_free(pkey);
            spdlog::error("Failed to create EVP_PKEY_CTX");
            return {};
        }

        if (EVP_PKEY_keygen_init(ctx) <= 0 ||
            EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) <= 0 ||
            EVP_PKEY_keygen(ctx, &pkey) <= 0)
        {
            EVP_PKEY_CTX_free(ctx);
            EVP_PKEY_free(pkey);
            spdlog::error("Failed to generate RSA key");
            return {};
        }
        EVP_PKEY_CTX_free(ctx);

        // Create X509 certificate
        X509 *x509 = X509_new();
        if (!x509)
        {
            EVP_PKEY_free(pkey);
            spdlog::error("Failed to allocate X509");
            return {};
        }

        // Set version to V3
        X509_set_version(x509, 2);

        // Set serial number
        ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);

        // Set validity: now to +10 years
        X509_gmtime_adj(X509_get_notBefore(x509), 0);
        X509_gmtime_adj(X509_get_notAfter(x509), 10L * 365 * 24 * 3600);

        // Set public key
        X509_set_pubkey(x509, pkey);

        // Set subject name
        X509_NAME *name = X509_get_subject_name(x509);
        X509_NAME_add_entry_by_txt(name, "C", MBSTRING_ASC, (unsigned char *)"US", -1, -1, 0);
        X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, (unsigned char *)"StreaMonitor", -1, -1, 0);
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, (unsigned char *)"StreaMonitor Local Server", -1, -1, 0);

        // Self-signed: issuer = subject
        X509_set_issuer_name(x509, name);

        // Add Subject Alternative Names — detect current LAN IP
        X509V3_CTX v3ctx;
        X509V3_set_ctx_nodb(&v3ctx);
        X509V3_set_ctx(&v3ctx, x509, x509, nullptr, nullptr, 0);

        // Build SAN string with localhost + detected LAN IPs
        std::string san_str = "DNS:localhost,DNS:*.local,IP:127.0.0.1,IP:0.0.0.0";

        // Detect all private IPs and add them
#ifdef _WIN32
        {
            ULONG bufLen = 15000;
            std::vector<BYTE> addrBuf(bufLen);
            auto pAddr = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(addrBuf.data());
            DWORD ret = GetAdaptersAddresses(AF_INET,
                                             GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                                             nullptr, pAddr, &bufLen);
            if (ret == ERROR_BUFFER_OVERFLOW)
            {
                addrBuf.resize(bufLen);
                pAddr = reinterpret_cast<PIP_ADAPTER_ADDRESSES>(addrBuf.data());
                ret = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST,
                                           nullptr, pAddr, &bufLen);
            }
            if (ret == NO_ERROR)
            {
                for (auto a = pAddr; a; a = a->Next)
                {
                    if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK)
                        continue;
                    for (auto u = a->FirstUnicastAddress; u; u = u->Next)
                    {
                        if (u->Address.lpSockaddr->sa_family != AF_INET)
                            continue;
                        auto sa = reinterpret_cast<sockaddr_in *>(u->Address.lpSockaddr);
                        char ip[INET_ADDRSTRLEN];
                        inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
                        std::string ipStr(ip);
                        if (ipStr != "127.0.0.1" && ipStr.substr(0, 4) != "169.")
                        {
                            san_str += ",IP:" + ipStr;
                            spdlog::info("SSL SAN: added LAN IP {}", ipStr);
                        }
                    }
                }
            }
        }
#else
        {
            struct ifaddrs *ifaddr;
            if (getifaddrs(&ifaddr) == 0)
            {
                for (auto ifa = ifaddr; ifa; ifa = ifa->ifa_next)
                {
                    if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
                        continue;
                    auto sa = reinterpret_cast<sockaddr_in *>(ifa->ifa_addr);
                    char ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
                    std::string ipStr(ip);
                    if (ipStr != "127.0.0.1" && ipStr.substr(0, 4) != "169.")
                    {
                        san_str += ",IP:" + ipStr;
                        spdlog::info("SSL SAN: added LAN IP {}", ipStr);
                    }
                }
                freeifaddrs(ifaddr);
            }
        }
#endif

        X509_EXTENSION *san = X509V3_EXT_conf_nid(nullptr, &v3ctx, NID_subject_alt_name,
                                                  const_cast<char *>(san_str.c_str()));
        if (san)
        {
            X509_add_ext(x509, san, -1);
            X509_EXTENSION_free(san);
        }

        // Sign the certificate with our key
        if (!X509_sign(x509, pkey, EVP_sha256()))
        {
            X509_free(x509);
            EVP_PKEY_free(pkey);
            spdlog::error("Failed to sign certificate");
            return {};
        }

        // Ensure directory exists
        fs::create_directories(fs::path(certDir));

        // Write certificate PEM
        FILE *certFile = fopen(paths.certPath.c_str(), "wb");
        if (certFile)
        {
            PEM_write_X509(certFile, x509);
            fclose(certFile);
        }
        else
        {
            spdlog::error("Failed to write certificate to {}", paths.certPath);
        }

        // Write private key PEM
        FILE *keyFile = fopen(paths.keyPath.c_str(), "wb");
        if (keyFile)
        {
            PEM_write_PrivateKey(keyFile, pkey, nullptr, nullptr, 0, nullptr, nullptr);
            fclose(keyFile);
        }
        else
        {
            spdlog::error("Failed to write private key to {}", paths.keyPath);
        }

        X509_free(x509);
        EVP_PKEY_free(pkey);

        spdlog::info("SSL certificate generated: {}", paths.certPath);

        // Auto-install the new cert to system trust store
        installCertToTrustStore(paths.certPath);

        return paths;
    }

} // namespace sm
