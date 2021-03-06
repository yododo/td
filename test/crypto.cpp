
#include <stdio.h>
#include <openssl/evp.h>
#include <openssl/bio.h>
#include <openssl/pem.h>

#include "td/utils/Status.h"
#include "td/utils/SharedSlice.h"
#include "td/utils/base64.h"
#include "td/utils/tl_storers.h"

#include "td/utils/crypto.h"

#include "td/mtproto/AuthKey.h"
#include "td/mtproto/KDF.h"
#include "td/mtproto/Transport.h"


class Handshake {
 public:
  struct KeyPair {
    td::SecureString private_key;
    td::SecureString public_key;
  };

  static td::Result<KeyPair> generate_key_pair() {
    EVP_PKEY *pkey = NULL;
    EVP_PKEY_CTX *pctx = EVP_PKEY_CTX_new_id(NID_X25519, NULL);
    SCOPE_EXIT {
      EVP_PKEY_CTX_free(pctx);
    };
    if (pctx == nullptr) {
      return td::Status::Error("Can't create EXP_PKEY_CTX");
    }
    if (EVP_PKEY_keygen_init(pctx) <= 0) {
      return td::Status::Error("Can't init keygen");
    }
    if (EVP_PKEY_keygen(pctx, &pkey) <= 0) {
      return td::Status::Error("Can't generate key");
    }

    TRY_RESULT(private_key, X25519_key_from_PKEY(pkey, true));
    TRY_RESULT(public_key, X25519_key_from_PKEY(pkey, false));

    KeyPair res;
    res.private_key = std::move(private_key);
    res.public_key = std::move(public_key);

    return res;
  }

  static td::SecureString expand_secret(td::Slice secret) {
    td::SecureString res(128);
    td::hmac_sha512(secret, "0", res.as_mutable_slice().substr(0, 64));
    td::hmac_sha512(secret, "1", res.as_mutable_slice().substr(64, 64));
    return res;
  }

  static td::Result<td::SecureString> privateKeyToPem(td::Slice key) {
    auto pkey_private = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, key.ubegin(), 32);
    CHECK(pkey_private != nullptr);
    auto res = X25519_pem_from_PKEY(pkey_private, true);
    EVP_PKEY_free(pkey_private);
    return res;
  }

  static td::Result<td::SecureString> calc_shared_secret(td::Slice private_key, td::Slice other_public_key) {
    auto pkey_private = EVP_PKEY_new_raw_private_key(EVP_PKEY_X25519, nullptr, private_key.ubegin(), 32);
    if (pkey_private == nullptr) {
      return td::Status::Error("Invalid X25520 private key");
    }
    SCOPE_EXIT {
      EVP_PKEY_free(pkey_private);
    };

    auto pkey_public =
        EVP_PKEY_new_raw_public_key(EVP_PKEY_X25519, nullptr, other_public_key.ubegin(), other_public_key.size());
    if (pkey_public == nullptr) {
      return td::Status::Error("Invalid X25519 public key");
    }
    SCOPE_EXIT {
      EVP_PKEY_free(pkey_public);
    };

    EVP_PKEY_CTX *ctx = EVP_PKEY_CTX_new(pkey_private, nullptr);
    if (ctx == nullptr) {
      return td::Status::Error("Can't create EVP_PKEY_CTX");
    }
    SCOPE_EXIT {
      EVP_PKEY_CTX_free(ctx);
    };

    if (EVP_PKEY_derive_init(ctx) <= 0) {
      return td::Status::Error("Can't init derive");
    }
    if (EVP_PKEY_derive_set_peer(ctx, pkey_public) <= 0) {
      return td::Status::Error("Can't init derive");
    }

    size_t result_len = 0;
    if (EVP_PKEY_derive(ctx, nullptr, &result_len) <= 0) {
      return td::Status::Error("Can't get result length");
    }
    if (result_len != 32) {
      return td::Status::Error("Unexpected result length");
    }

    td::SecureString result(result_len, '\0');
    if (EVP_PKEY_derive(ctx, result.as_mutable_slice().ubegin(), &result_len) <= 0) {
      return td::Status::Error("Failed to compute shared secret");
    }
    return std::move(result);
  }

 private:
  static td::Result<td::SecureString> X25519_key_from_PKEY(EVP_PKEY *pkey, bool is_private) {
    auto func = is_private ? &EVP_PKEY_get_raw_private_key : &EVP_PKEY_get_raw_public_key;
    size_t len = 0;
    if (func(pkey, nullptr, &len) == 0) {
      return td::Status::Error("Failed to get raw key length");
    }
    CHECK(len == 32);

    td::SecureString result(len);
    if (func(pkey, result.as_mutable_slice().ubegin(), &len) == 0) {
      return td::Status::Error("Failed to get raw key");
    }
    return std::move(result);
  }
  static td::Result<td::SecureString> X25519_pem_from_PKEY(EVP_PKEY *pkey, bool is_private) {
    BIO *mem_bio = BIO_new(BIO_s_mem());
    SCOPE_EXIT {
      BIO_vfree(mem_bio);
    };
    if (is_private) {
      PEM_write_bio_PrivateKey(mem_bio, pkey, nullptr, nullptr, 0, nullptr, nullptr);
    } else {
      PEM_write_bio_PUBKEY(mem_bio, pkey);
    }
    char *data_ptr = nullptr;
    auto data_size = BIO_get_mem_data(mem_bio, &data_ptr);
    return std::string(data_ptr, data_size);
  }
};

struct HandshakeTest {
  Handshake::KeyPair alice;
  Handshake::KeyPair bob;

  td::SecureString shared_secret;
  td::SecureString key;
};

namespace td {

void KDF2(Slice auth_key, const UInt128 &msg_key, int X, UInt256 *aes_key, UInt128 *aes_iv) {
  uint8 buf_raw[36 + 16];
  MutableSlice buf(buf_raw, 36 + 16);
  Slice msg_key_slice = as_slice(msg_key);

  // sha256_a = SHA256 (msg_key + substr(auth_key, x, 36));
  buf.copy_from(msg_key_slice);
  buf.substr(16, 36).copy_from(auth_key.substr(X, 36));
  uint8 sha256_a_raw[32];
  MutableSlice sha256_a(sha256_a_raw, 32);
  sha256(buf, sha256_a);

  // sha256_b = SHA256 (substr(auth_key, 40+x, 36) + msg_key);
  buf.copy_from(auth_key.substr(40 + X, 36));
  buf.substr(36).copy_from(msg_key_slice);
  uint8 sha256_b_raw[32];
  MutableSlice sha256_b(sha256_b_raw, 32);
  sha256(buf, sha256_b);

  // aes_key = substr(sha256_a, 0, 8) + substr(sha256_b, 8, 16) + substr(sha256_a, 24, 8);
  MutableSlice aes_key_slice(aes_key->raw, sizeof(aes_key->raw));
  aes_key_slice.copy_from(sha256_a.substr(0, 8));
  aes_key_slice.substr(8).copy_from(sha256_b.substr(8, 16));
  aes_key_slice.substr(24).copy_from(sha256_a.substr(24, 8));

  // aes_iv = substr(sha256_b, 0, 4) + substr(sha256_a, 8, 8) + substr(sha256_b, 24, 4);
  MutableSlice aes_iv_slice(aes_iv->raw, sizeof(aes_iv->raw));
  aes_iv_slice.copy_from(sha256_b.substr(0, 4));
  aes_iv_slice.substr(4).copy_from(sha256_a.substr(8, 8));
  aes_iv_slice.substr(12).copy_from(sha256_b.substr(24, 4));
}
}

td::SecureString  encrypt(td::Slice key, td::Slice data, td::int32 seqno, int X) {
  td::SecureString res(data.size() + 4 + 16);
  res.as_mutable_slice().substr(20).copy_from(data);
  td::TlStorerUnsafe storer(res.as_mutable_slice().substr(16).ubegin());
  storer.store_int(seqno);
  td::mtproto::AuthKey auth_key(0, key.str());
  auto payload = res.as_mutable_slice().substr(16);
  td::UInt128  msg_key = td::mtproto::Transport::calc_message_key2(auth_key, X, payload).second;
  td::UInt256  aes_key;
  td::UInt128  aes_iv;
  td::KDF2(key, msg_key, X, &aes_key, &aes_iv);
  td::AesCtrState aes;
  aes.init(aes_key.as_slice(), aes_iv.as_slice());
  aes.encrypt(payload, payload);
  res.as_mutable_slice().copy_from(msg_key.as_slice());
  return res;
}

HandshakeTest gen_test() {
  HandshakeTest res;
  res.alice = Handshake::generate_key_pair().move_as_ok();

  res.bob = Handshake::generate_key_pair().move_as_ok();
  res.shared_secret = Handshake::calc_shared_secret(res.alice.private_key, res.bob.public_key).move_as_ok();
  res.key = Handshake::expand_secret(res.shared_secret);
  return res;
}


void run_test(const HandshakeTest &test) {
  auto alice_secret = Handshake::calc_shared_secret(test.alice.private_key, test.bob.public_key).move_as_ok();
  auto bob_secret = Handshake::calc_shared_secret(test.bob.private_key, test.alice.public_key).move_as_ok();
  auto key = Handshake::expand_secret(alice_secret);
  CHECK(alice_secret == bob_secret);
  CHECK(alice_secret == test.shared_secret);
  LOG(ERROR) << "Key\n\t" << td::base64url_encode(key) << "\n";
  CHECK(key == test.key);
}

td::StringBuilder &operator<<(td::StringBuilder &sb, const Handshake::KeyPair &key_pair) {
  sb << "\tpublic_key (base64url) = " << td::base64url_encode(key_pair.public_key) << "\n";
  sb << "\tprivate_key (base64url) = " << td::base64url_encode(key_pair.private_key) << "\n";
  sb << "\tprivate_key (pem) = \n" << Handshake::privateKeyToPem(key_pair.private_key).ok() << "\n";
  return sb;
}
td::StringBuilder &operator<<(td::StringBuilder &sb, const HandshakeTest &test) {
  sb << "Alice\n" << test.alice;
  sb << "Bob\n" << test.bob;
  sb << "SharedSecret\n\t" << td::base64url_encode(test.shared_secret) << "\n";
  sb << "Key\n\t" << td::base64url_encode(test.key) << "\n";

  std::string data = "hello world";
  sb << "encrypt(\"" << data << "\", X=0) = \n\t" << td::base64url_encode(encrypt(test.key, data, 1, 0)) << "\n";
  sb << "encrypt(\"" << data << "\", X=8) = \n\t" << td::base64url_encode(encrypt(test.key, data, 1, 8)) << "\n";
  return sb;
}

HandshakeTest pregenerated_test() {
  HandshakeTest test;
  test.alice.public_key = td::base64url_decode_secure("QlCME5fXLyyQQWeYnBiGAZbmzuD4ayOuADCFgmioOBY").move_as_ok();
  test.alice.private_key = td::base64url_decode_secure("8NZGWKfRCJfiks74RG9_xHmYydarLiRsoq8VcJGPglg").move_as_ok();
  test.bob.public_key = td::base64url_decode_secure("I1yzfmMCZzlI7xwMj1FJ3O3I3_aEUtv6CxbHiDGzr18").move_as_ok();
  test.bob.private_key = td::base64url_decode_secure("YMGoowtnZ99roUM2y5JRwiQrwGaNJ-ZRE5boy-l4aHg").move_as_ok();
  test.shared_secret = td::base64url_decode_secure("0IIvKJuXEwmAp41fYhjUnWqLTYQJ7QeKZKYuCG8mFz8").move_as_ok();
  test.key = td::base64url_decode_secure(
                 "JHmD-XW9j-13G6KP0tArIhQNDRVbRkKxx0MG0pa2nOgwJHNfiggM0I0RiNIr23-1wRboRtan4WvqOHsxAt_cngYX15_"
                 "HYe8tJdEwHcmlnXq7LtprigzExaNJS7skfOo2irClj-7EL06-jMrhfwngSJFsak8JFSw8s6R4fwCsr50")
                 .move_as_ok();


  return test;
}

int main() {
  auto test = gen_test();
  run_test(test);
  run_test(pregenerated_test());
  LOG(ERROR) << "\n" << pregenerated_test();
}
