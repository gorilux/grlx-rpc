#pragma once

// In-process X.509 test fixtures for grlx-rpc SSL tests.
//
// Generates a disposable CA, a server cert, and two client certs at process
// start using the OpenSSL C API. No PEM files are checked into the tree —
// every test run gets fresh material. Supports both EC P-256 keys (fast) and
// writes PEM strings that boost::asio::ssl::context accepts directly.

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <memory>
#include <stdexcept>
#include <string>

namespace grlx::rpc::testing {

struct openssl_deleter {
  void operator()(EVP_PKEY* p) const { EVP_PKEY_free(p); }
  void operator()(EVP_PKEY_CTX* p) const { EVP_PKEY_CTX_free(p); }
  void operator()(X509* p) const { X509_free(p); }
  void operator()(X509_NAME* p) const { X509_NAME_free(p); }
  void operator()(BIO* p) const { BIO_free_all(p); }
};

using evp_pkey_ptr     = std::unique_ptr<EVP_PKEY, openssl_deleter>;
using evp_pkey_ctx_ptr = std::unique_ptr<EVP_PKEY_CTX, openssl_deleter>;
using x509_ptr         = std::unique_ptr<X509, openssl_deleter>;
using bio_ptr          = std::unique_ptr<BIO, openssl_deleter>;

inline std::string openssl_last_error() {
  bio_ptr bio(BIO_new(BIO_s_mem()));
  ERR_print_errors(bio.get());
  char*  buf  = nullptr;
  long   len  = BIO_get_mem_data(bio.get(), &buf);
  return std::string(buf, static_cast<std::size_t>(len));
}

inline evp_pkey_ptr generate_ec_key() {
  evp_pkey_ctx_ptr ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr));
  if (!ctx || EVP_PKEY_keygen_init(ctx.get()) <= 0) {
    throw std::runtime_error("keygen init failed: " + openssl_last_error());
  }
  if (EVP_PKEY_CTX_set_ec_paramgen_curve_nid(ctx.get(), NID_X9_62_prime256v1) <= 0) {
    throw std::runtime_error("set curve failed: " + openssl_last_error());
  }
  EVP_PKEY* raw = nullptr;
  if (EVP_PKEY_keygen(ctx.get(), &raw) <= 0) {
    throw std::runtime_error("keygen failed: " + openssl_last_error());
  }
  return evp_pkey_ptr(raw);
}

inline void add_ext(X509* cert, int nid, char const* value) {
  X509V3_CTX ctx;
  X509V3_set_ctx_nodb(&ctx);
  X509V3_set_ctx(&ctx, cert, cert, nullptr, nullptr, 0);
  X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &ctx, nid, value);
  if (!ext) {
    throw std::runtime_error("X509V3_EXT_conf_nid failed for nid " + std::to_string(nid) + ": " + openssl_last_error());
  }
  X509_add_ext(cert, ext, -1);
  X509_EXTENSION_free(ext);
}

inline x509_ptr build_cert(std::string const& cn,
                           EVP_PKEY*          subject_key,
                           X509*              issuer_cert,
                           EVP_PKEY*          issuer_key,
                           bool               is_ca,
                           bool               is_server) {
  x509_ptr cert(X509_new());
  X509_set_version(cert.get(), 2);

  // Use nanoseconds-derived serial to keep serials unique across certs in the same run.
  static unsigned long long serial_counter = 1;
  ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), static_cast<long>(++serial_counter));

  X509_gmtime_adj(X509_getm_notBefore(cert.get()), 0);
  X509_gmtime_adj(X509_getm_notAfter(cert.get()), 60L * 60L);  // 1 hour — enough for any test run

  X509_set_pubkey(cert.get(), subject_key);

  X509_NAME* name = X509_get_subject_name(cert.get());
  X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                             reinterpret_cast<unsigned char const*>(cn.c_str()), -1, -1, 0);

  X509_NAME* issuer_name = issuer_cert ? X509_get_subject_name(issuer_cert) : name;
  X509_set_issuer_name(cert.get(), issuer_name);

  if (is_ca) {
    add_ext(cert.get(), NID_basic_constraints, "critical,CA:TRUE");
    add_ext(cert.get(), NID_key_usage, "critical,keyCertSign,cRLSign");
  } else {
    add_ext(cert.get(), NID_basic_constraints, "critical,CA:FALSE");
    add_ext(cert.get(), NID_key_usage, "critical,digitalSignature,keyEncipherment");
    if (is_server) {
      add_ext(cert.get(), NID_ext_key_usage, "serverAuth");
      add_ext(cert.get(), NID_subject_alt_name, "DNS:localhost,IP:127.0.0.1,IP:::1");
    } else {
      add_ext(cert.get(), NID_ext_key_usage, "clientAuth");
    }
  }

  if (!X509_sign(cert.get(), issuer_key, EVP_sha256())) {
    throw std::runtime_error("X509_sign failed: " + openssl_last_error());
  }
  return cert;
}

inline std::string to_pem(X509* cert) {
  bio_ptr bio(BIO_new(BIO_s_mem()));
  PEM_write_bio_X509(bio.get(), cert);
  char*  buf;
  long   len = BIO_get_mem_data(bio.get(), &buf);
  return std::string(buf, static_cast<std::size_t>(len));
}

inline std::string to_pem(EVP_PKEY* key) {
  bio_ptr bio(BIO_new(BIO_s_mem()));
  PEM_write_bio_PrivateKey(bio.get(), key, nullptr, nullptr, 0, nullptr, nullptr);
  char*  buf;
  long   len = BIO_get_mem_data(bio.get(), &buf);
  return std::string(buf, static_cast<std::size_t>(len));
}

struct pem_bundle {
  std::string cert_pem;
  std::string key_pem;
};

// Everything needed to stand up an mTLS server+client pair in a test:
//   - primary_ca: trusted CA used by both server and real clients
//   - server:    cert/key for the server, signed by primary_ca
//   - client_a:  cert/key signed by primary_ca (identity A)
//   - client_b:  cert/key signed by primary_ca (identity B — different CN)
//   - rogue_ca + rogue_client: cert signed by an untrusted CA (negative test)
struct cert_set {
  pem_bundle primary_ca;
  pem_bundle server;
  pem_bundle client_a;
  pem_bundle client_b;
  pem_bundle rogue_ca;
  pem_bundle rogue_client;
};

inline cert_set const& shared_cert_set() {
  static cert_set const certs = []() {
    cert_set out;

    // Primary trust anchor
    auto ca_key   = generate_ec_key();
    auto ca_cert  = build_cert("grlx-rpc-test-ca", ca_key.get(), nullptr, ca_key.get(), true, false);
    out.primary_ca.cert_pem = to_pem(ca_cert.get());
    out.primary_ca.key_pem  = to_pem(ca_key.get());

    // Server cert signed by primary CA
    {
      auto key  = generate_ec_key();
      auto cert = build_cert("localhost", key.get(), ca_cert.get(), ca_key.get(), false, true);
      out.server.cert_pem = to_pem(cert.get());
      out.server.key_pem  = to_pem(key.get());
    }

    // Two distinct client identities, both signed by primary CA
    for (auto [cn, slot] : std::initializer_list<std::pair<char const*, pem_bundle*>>{
             {"client-a", &out.client_a}, {"client-b", &out.client_b}}) {
      auto key  = generate_ec_key();
      auto cert = build_cert(cn, key.get(), ca_cert.get(), ca_key.get(), false, false);
      slot->cert_pem = to_pem(cert.get());
      slot->key_pem  = to_pem(key.get());
    }

    // Rogue CA + client for negative mTLS tests
    auto rogue_ca_key  = generate_ec_key();
    auto rogue_ca_cert = build_cert("rogue-ca", rogue_ca_key.get(), nullptr, rogue_ca_key.get(), true, false);
    out.rogue_ca.cert_pem = to_pem(rogue_ca_cert.get());
    out.rogue_ca.key_pem  = to_pem(rogue_ca_key.get());
    {
      auto key  = generate_ec_key();
      auto cert = build_cert("rogue-client", key.get(), rogue_ca_cert.get(), rogue_ca_key.get(), false, false);
      out.rogue_client.cert_pem = to_pem(cert.get());
      out.rogue_client.key_pem  = to_pem(key.get());
    }
    return out;
  }();
  return certs;
}

}  // namespace grlx::rpc::testing
