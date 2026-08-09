#include "engine/tls/certificate_manager.hpp"

#include "common/constants.hpp"

#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <shlobj.h>
#include <wincrypt.h>

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <vector>

namespace appytizer {
namespace {
constexpr wchar_t kCaCertificateName[] = L"appytizer-root-ca.crt.pem";
constexpr wchar_t kCaPrivateKeyName[] = L"appytizer-root-ca.key.pem";
constexpr wchar_t kMetadataName[] = L"ownership.json";
constexpr int kRootValidityDays = 3652;
constexpr int kLeafValidityDays = 397;
constexpr int kRenewalDays = 30;

template <typename T, auto Free>
struct OpenSslDeleter {
  void operator()(T* value) const {
    if (value) {
      Free(value);
    }
  }
};
template <typename T, auto Free>
using OpenSslPtr = std::unique_ptr<T, OpenSslDeleter<T, Free>>;
using BioPtr = OpenSslPtr<BIO, BIO_free>;
using KeyPtr = OpenSslPtr<EVP_PKEY, EVP_PKEY_free>;
using CertificatePtr = OpenSslPtr<X509, X509_free>;
using KeyContextPtr = OpenSslPtr<EVP_PKEY_CTX, EVP_PKEY_CTX_free>;
using NamesPtr = OpenSslPtr<GENERAL_NAMES, GENERAL_NAMES_free>;
using BigNumberPtr = OpenSslPtr<BIGNUM, BN_free>;

std::filesystem::path ca_certificate_path(const std::filesystem::path& directory) {
  return directory / kCaCertificateName;
}

std::filesystem::path ca_private_key_path(const std::filesystem::path& directory) {
  return directory / kCaPrivateKeyName;
}

std::filesystem::path metadata_path(const std::filesystem::path& directory) {
  return directory / kMetadataName;
}

std::string read_file(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  return input ? std::string(std::istreambuf_iterator<char>(input), {}) : std::string{};
}

bool restrict_private_key(const std::filesystem::path& path);

bool atomic_write(const std::filesystem::path& path, std::string_view contents,
                  bool restrict_access = false) {
  static std::atomic_uint64_t next{1};
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (error) {
    return false;
  }
  const auto temporary = path.wstring() + L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." +
                         std::to_wstring(next++);
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
      return false;
    }
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    output.flush();
    if (!output) {
      output.close();
      std::filesystem::remove(temporary, error);
      return false;
    }
  }
  if (restrict_access && !restrict_private_key(temporary)) {
    std::filesystem::remove(temporary, error);
    return false;
  }
  if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    std::filesystem::remove(temporary, error);
    return false;
  }
  return true;
}

bool restrict_private_key(const std::filesystem::path& path) {
  PSECURITY_DESCRIPTOR descriptor{};
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
          L"D:P(A;;FA;;;SY)(A;;FA;;;BA)", SDDL_REVISION_1, &descriptor, nullptr)) {
    return false;
  }
  BOOL present{};
  BOOL defaulted{};
  PACL acl{};
  const bool obtained = GetSecurityDescriptorDacl(descriptor, &present, &acl, &defaulted) != FALSE;
  const DWORD result = obtained && present
                           ? SetNamedSecurityInfoW(const_cast<wchar_t*>(path.c_str()), SE_FILE_OBJECT,
                                                   DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                                   nullptr, nullptr, acl, nullptr)
                           : ERROR_INVALID_SECURITY_DESCR;
  LocalFree(descriptor);
  return result == ERROR_SUCCESS;
}

CertificatePtr load_certificate(const std::filesystem::path& path) {
  const auto pem = read_file(path);
  BioPtr bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
  return CertificatePtr(bio ? PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr) : nullptr);
}

KeyPtr load_private_key(const std::filesystem::path& path) {
  const auto pem = read_file(path);
  BioPtr bio(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
  return KeyPtr(bio ? PEM_read_bio_PrivateKey(bio.get(), nullptr, nullptr, nullptr) : nullptr);
}

std::string certificate_pem(X509* certificate) {
  BioPtr bio(BIO_new(BIO_s_mem()));
  if (!bio || PEM_write_bio_X509(bio.get(), certificate) != 1) {
    return {};
  }
  BUF_MEM* memory{};
  BIO_get_mem_ptr(bio.get(), &memory);
  return memory ? std::string(memory->data, memory->length) : std::string{};
}

std::string private_key_pem(EVP_PKEY* key) {
  BioPtr bio(BIO_new(BIO_s_mem()));
  if (!bio || PEM_write_bio_PrivateKey(bio.get(), key, nullptr, nullptr, 0, nullptr, nullptr) != 1) {
    return {};
  }
  BUF_MEM* memory{};
  BIO_get_mem_ptr(bio.get(), &memory);
  return memory ? std::string(memory->data, memory->length) : std::string{};
}

KeyPtr generate_rsa_key(int bits) {
  KeyContextPtr context(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr));
  EVP_PKEY* raw{};
  if (!context || EVP_PKEY_keygen_init(context.get()) <= 0 ||
      EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), bits) <= 0 ||
      EVP_PKEY_keygen(context.get(), &raw) <= 0) {
    return KeyPtr(nullptr);
  }
  return KeyPtr(raw);
}

bool set_random_serial(X509* certificate) {
  BigNumberPtr number(BN_new());
  if (!number || BN_rand(number.get(), 128, BN_RAND_TOP_ONE, BN_RAND_BOTTOM_ANY) != 1) {
    return false;
  }
  ASN1_INTEGER* serial = BN_to_ASN1_INTEGER(number.get(), nullptr);
  if (!serial) {
    return false;
  }
  const bool result = X509_set_serialNumber(certificate, serial) == 1;
  ASN1_INTEGER_free(serial);
  return result;
}

bool add_extension(X509* certificate, X509* issuer, int nid, std::string_view value) {
  X509V3_CTX context{};
  X509V3_set_ctx(&context, issuer, certificate, nullptr, nullptr, 0);
  X509_EXTENSION* extension = X509V3_EXT_conf_nid(
      nullptr, &context, nid, const_cast<char*>(std::string(value).c_str()));
  if (!extension) {
    return false;
  }
  const bool added = X509_add_ext(certificate, extension, -1) == 1;
  X509_EXTENSION_free(extension);
  return added;
}

bool set_common_name(X509_NAME* name, std::string_view common_name) {
  return X509_NAME_add_entry_by_NID(name, NID_commonName, MBSTRING_UTF8,
                                    reinterpret_cast<const unsigned char*>(common_name.data()),
                                    static_cast<int>(common_name.size()), -1, 0) == 1;
}

bool create_root(const std::filesystem::path& directory, bool restrict_private_keys) {
  auto key = generate_rsa_key(3072);
  CertificatePtr certificate(X509_new());
  if (!key || !certificate || X509_set_version(certificate.get(), 2) != 1 ||
      !set_random_serial(certificate.get()) || X509_gmtime_adj(X509_getm_notBefore(certificate.get()), 0) == nullptr ||
      X509_gmtime_adj(X509_getm_notAfter(certificate.get()),
                      static_cast<long>(kRootValidityDays) * 24 * 60 * 60) == nullptr ||
      X509_set_pubkey(certificate.get(), key.get()) != 1) {
    return false;
  }
  X509_NAME* subject = X509_get_subject_name(certificate.get());
  if (!subject || !set_common_name(subject, "Appytizer Local Development Root CA") ||
      X509_set_issuer_name(certificate.get(), subject) != 1 ||
      !add_extension(certificate.get(), certificate.get(), NID_basic_constraints, "critical,CA:TRUE,pathlen:0") ||
      !add_extension(certificate.get(), certificate.get(), NID_key_usage, "critical,keyCertSign,cRLSign") ||
      !add_extension(certificate.get(), certificate.get(), NID_subject_key_identifier, "hash") ||
      !add_extension(certificate.get(), certificate.get(), NID_authority_key_identifier, "keyid:always") ||
      X509_sign(certificate.get(), key.get(), EVP_sha256()) <= 0) {
    return false;
  }
  const auto key_text = private_key_pem(key.get());
  const auto certificate_text = certificate_pem(certificate.get());
  const auto key_path = ca_private_key_path(directory);
  if (key_text.empty() || certificate_text.empty() ||
      !atomic_write(key_path, key_text, restrict_private_keys) ||
      !atomic_write(ca_certificate_path(directory), certificate_text)) {
    return false;
  }
  return true;
}

std::vector<unsigned char> certificate_der(X509* certificate) {
  const int size = i2d_X509(certificate, nullptr);
  if (size <= 0) {
    return {};
  }
  std::vector<unsigned char> der(static_cast<std::size_t>(size));
  unsigned char* cursor = der.data();
  i2d_X509(certificate, &cursor);
  return der;
}

std::string digest_hex(X509* certificate, const EVP_MD* digest) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> bytes{};
  unsigned int size{};
  if (X509_digest(certificate, digest, bytes.data(), &size) != 1) {
    return {};
  }
  std::ostringstream output;
  output << std::uppercase << std::hex << std::setfill('0');
  for (unsigned int index = 0; index < size; ++index) {
    output << std::setw(2) << static_cast<unsigned int>(bytes[index]);
  }
  return output.str();
}

bool save_metadata(const std::filesystem::path& directory, X509* certificate) {
  const nlohmann::json metadata{{"owner", "Appytizer"},
                                {"sha1_thumbprint", digest_hex(certificate, EVP_sha1())},
                                {"sha256", digest_hex(certificate, EVP_sha256())}};
  return atomic_write(metadata_path(directory), metadata.dump(2) + "\n");
}

std::optional<nlohmann::json> load_metadata(const std::filesystem::path& directory) {
  try {
    const auto json = nlohmann::json::parse(read_file(metadata_path(directory)));
    if (json.value("owner", "") != "Appytizer") {
      return std::nullopt;
    }
    return std::make_optional<nlohmann::json>(json);
  } catch (...) {
    return std::nullopt;
  }
}

bool is_ca_valid(X509* certificate, EVP_PKEY* key) {
  if (!certificate || !key || EVP_PKEY_bits(key) < 3072 || X509_check_private_key(certificate, key) != 1 ||
      X509_check_ca(certificate) <= 0 || X509_cmp_current_time(X509_get0_notAfter(certificate)) <= 0 ||
      X509_verify(certificate, key) != 1) {
    return false;
  }
  return X509_NAME_cmp(X509_get_subject_name(certificate), X509_get_issuer_name(certificate)) == 0;
}

bool valid_hostname(std::string_view hostname) {
  if (!hostname.ends_with(kSiteSuffix) || hostname.size() <= kSiteSuffix.size()) {
    return false;
  }
  const auto label = hostname.substr(0, hostname.size() - kSiteSuffix.size());
  if (label.empty() || label.size() > 63 || label.front() == '-' || label.back() == '-') {
    return false;
  }
  return std::ranges::all_of(label, [](unsigned char value) {
    return std::islower(value) != 0 || std::isdigit(value) != 0 || value == '-';
  });
}

bool has_exact_server_auth(X509* certificate) {
  EXTENDED_KEY_USAGE* usage = static_cast<EXTENDED_KEY_USAGE*>(
      X509_get_ext_d2i(certificate, NID_ext_key_usage, nullptr, nullptr));
  if (!usage) {
    return false;
  }
  bool found = false;
  for (int index = 0; index < sk_ASN1_OBJECT_num(usage); ++index) {
    if (OBJ_obj2nid(sk_ASN1_OBJECT_value(usage, index)) == NID_server_auth) {
      found = true;
    }
  }
  EXTENDED_KEY_USAGE_free(usage);
  return found;
}

bool has_exact_san(X509* certificate, std::string_view hostname) {
  NamesPtr names(static_cast<GENERAL_NAMES*>(X509_get_ext_d2i(certificate, NID_subject_alt_name, nullptr, nullptr)));
  if (!names || sk_GENERAL_NAME_num(names.get()) != 1) {
    return false;
  }
  const GENERAL_NAME* name = sk_GENERAL_NAME_value(names.get(), 0);
  if (!name || name->type != GEN_DNS) {
    return false;
  }
  const auto* bytes = ASN1_STRING_get0_data(name->d.dNSName);
  const int size = ASN1_STRING_length(name->d.dNSName);
  return size == static_cast<int>(hostname.size()) &&
         std::string_view(reinterpret_cast<const char*>(bytes), static_cast<std::size_t>(size)) == hostname;
}

bool expires_within(X509* certificate, int days) {
  int remaining_days{};
  int remaining_seconds{};
  if (ASN1_TIME_diff(&remaining_days, &remaining_seconds, nullptr, X509_get0_notAfter(certificate)) != 1) {
    return true;
  }
  return remaining_days < days || (remaining_days == days && remaining_seconds <= 0);
}

std::string expiry_text(X509* certificate) {
  std::tm value{};
  if (!certificate || ASN1_TIME_to_tm(X509_get0_notAfter(certificate), &value) != 1) {
    return {};
  }
  std::ostringstream output;
  output << std::put_time(&value, "%Y-%m-%dT%H:%M:%SZ");
  return output.str();
}

bool create_leaf(const std::filesystem::path& directory, std::string_view hostname,
                 bool restrict_private_keys) {
  auto ca_certificate = load_certificate(ca_certificate_path(directory));
  auto ca_key = load_private_key(ca_private_key_path(directory));
  auto key = generate_rsa_key(2048);
  CertificatePtr certificate(X509_new());
  if (!is_ca_valid(ca_certificate.get(), ca_key.get()) || !key || !certificate ||
      X509_set_version(certificate.get(), 2) != 1 || !set_random_serial(certificate.get()) ||
      X509_gmtime_adj(X509_getm_notBefore(certificate.get()), 0) == nullptr ||
      X509_gmtime_adj(X509_getm_notAfter(certificate.get()),
                      static_cast<long>(kLeafValidityDays) * 24 * 60 * 60) == nullptr ||
      X509_set_pubkey(certificate.get(), key.get()) != 1) {
    return false;
  }
  X509_NAME* subject = X509_get_subject_name(certificate.get());
  if (!subject || !set_common_name(subject, hostname) ||
      X509_set_issuer_name(certificate.get(), X509_get_subject_name(ca_certificate.get())) != 1 ||
      !add_extension(certificate.get(), ca_certificate.get(), NID_basic_constraints, "critical,CA:FALSE") ||
      !add_extension(certificate.get(), ca_certificate.get(), NID_key_usage, "critical,digitalSignature,keyEncipherment") ||
      !add_extension(certificate.get(), ca_certificate.get(), NID_ext_key_usage, "serverAuth") ||
      !add_extension(certificate.get(), ca_certificate.get(), NID_subject_alt_name,
                     std::string("DNS:") + std::string(hostname)) ||
      !add_extension(certificate.get(), ca_certificate.get(), NID_subject_key_identifier, "hash") ||
      !add_extension(certificate.get(), ca_certificate.get(), NID_authority_key_identifier, "keyid,issuer") ||
      X509_sign(certificate.get(), ca_key.get(), EVP_sha256()) <= 0) {
    return false;
  }
  const auto sites = directory / L"sites";
  const auto key_path = sites / (std::string(hostname) + ".key.pem");
  const auto certificate_path = sites / (std::string(hostname) + ".crt.pem");
  const auto key_text = private_key_pem(key.get());
  const auto certificate_text = certificate_pem(certificate.get());
  return !key_text.empty() && !certificate_text.empty() &&
         atomic_write(key_path, key_text, restrict_private_keys) &&
         atomic_write(certificate_path, certificate_text);
}

HCERTSTORE open_root_store(DWORD access) {
  return CertOpenStore(CERT_STORE_PROV_SYSTEM_W, 0, 0,
                       CERT_SYSTEM_STORE_LOCAL_MACHINE | CERT_STORE_OPEN_EXISTING_FLAG | access, L"ROOT");
}

bool store_contains_der(HCERTSTORE store, const std::vector<unsigned char>& der) {
  PCCERT_CONTEXT current{};
  while ((current = CertEnumCertificatesInStore(store, current)) != nullptr) {
    if (current->cbCertEncoded == der.size() &&
        std::equal(der.begin(), der.end(), current->pbCertEncoded)) {
      return true;
    }
  }
  return false;
}

std::string context_digest_hex(PCCERT_CONTEXT context, ALG_ID algorithm) {
  std::array<BYTE, 64> digest{};
  DWORD size = static_cast<DWORD>(digest.size());
  if (!CryptHashCertificate(0, algorithm, 0, context->pbCertEncoded, context->cbCertEncoded,
                            digest.data(), &size)) {
    return {};
  }
  std::ostringstream output;
  output << std::uppercase << std::hex << std::setfill('0');
  for (DWORD index = 0; index < size; ++index) {
    output << std::setw(2) << static_cast<unsigned int>(digest[index]);
  }
  return output.str();
}

} // namespace

void to_json(nlohmann::json& json, const TlsStatus& status) {
  json = {{"ready", status.ready},
          {"trusted", status.trusted},
          {"site_certificate_count", status.site_certificate_count},
          {"earliest_expiry", status.earliest_expiry},
          {"error", status.error}};
}

CertificateManager::CertificateManager(std::filesystem::path directory, bool restrict_private_keys)
    : directory_(std::move(directory)), restrict_private_keys_(restrict_private_keys) {}

std::filesystem::path CertificateManager::default_directory() {
  if (const DWORD size = GetEnvironmentVariableW(L"APPYTIZER_CERTIFICATES_DIR", nullptr, 0); size > 0) {
    std::wstring value(size, L'\0');
    GetEnvironmentVariableW(L"APPYTIZER_CERTIFICATES_DIR", value.data(), size);
    value.resize(wcslen(value.c_str()));
    return value;
  }
  PWSTR raw{};
  std::filesystem::path program_data = L"C:\\ProgramData";
  if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, KF_FLAG_CREATE, nullptr, &raw))) {
    program_data = raw;
    CoTaskMemFree(raw);
  }
  return program_data / kApplicationId / L"certificates";
}

std::filesystem::path CertificateManager::certificate_path(std::string_view hostname) const {
  return directory_ / L"sites" / (std::string(hostname) + ".crt.pem");
}

std::filesystem::path CertificateManager::private_key_path(std::string_view hostname) const {
  return directory_ / L"sites" / (std::string(hostname) + ".key.pem");
}

void CertificateManager::set_error(std::string message) const {
  last_error_ = std::move(message);
}

bool CertificateManager::ca_files_valid() const {
  auto certificate = load_certificate(ca_certificate_path(directory_));
  auto key = load_private_key(ca_private_key_path(directory_));
  return is_ca_valid(certificate.get(), key.get());
}

bool CertificateManager::ensure_ca_files() {
  if (ca_files_valid()) {
    auto certificate = load_certificate(ca_certificate_path(directory_));
    const auto metadata = load_metadata(directory_);
    const bool metadata_matches = metadata &&
        metadata->value("sha1_thumbprint", "") == digest_hex(certificate.get(), EVP_sha1()) &&
        metadata->value("sha256", "") == digest_hex(certificate.get(), EVP_sha256());
    if (!metadata_matches && !save_metadata(directory_, certificate.get())) {
      set_error("Could not record Appytizer CA ownership metadata.");
      return false;
    }
    return true;
  }
  if (load_metadata(directory_)) {
    if (!remove_recorded_trust_entry()) {
      return false;
    }
  }
  std::error_code error;
  std::filesystem::create_directories(directory_, error);
  if (error || !create_root(directory_, restrict_private_keys_)) {
    set_error("Could not create the Appytizer root CA files.");
    return false;
  }
  auto certificate = load_certificate(ca_certificate_path(directory_));
  if (!certificate || !save_metadata(directory_, certificate.get())) {
    set_error("Could not record Appytizer CA ownership metadata.");
    return false;
  }
  return true;
}

bool CertificateManager::install_trust_entry() {
  auto certificate = load_certificate(ca_certificate_path(directory_));
  const auto der = certificate ? certificate_der(certificate.get()) : std::vector<unsigned char>{};
  HCERTSTORE store = open_root_store(CERT_STORE_MAXIMUM_ALLOWED_FLAG);
  if (!store || der.empty()) {
    if (store) {
      CertCloseStore(store, 0);
    }
    set_error("Could not open the LocalMachine root certificate store. Run TLS repair as administrator.");
    return false;
  }
  bool installed = store_contains_der(store, der);
  if (!installed) {
    installed = CertAddEncodedCertificateToStore(store, X509_ASN_ENCODING, der.data(),
                                                  static_cast<DWORD>(der.size()),
                                                  CERT_STORE_ADD_NEW, nullptr) != FALSE;
  }
  CertCloseStore(store, 0);
  if (!installed) {
    set_error("Could not trust the Appytizer root CA in LocalMachine\\Root.");
  }
  return installed;
}

bool CertificateManager::provision() {
  std::scoped_lock lock(mutex_);
  last_error_.clear();
  return ensure_ca_files() && install_trust_entry();
}

bool CertificateManager::provision_files_only() {
  std::scoped_lock lock(mutex_);
  last_error_.clear();
  return ensure_ca_files();
}

bool CertificateManager::is_trusted() const {
  auto certificate = load_certificate(ca_certificate_path(directory_));
  const auto der = certificate ? certificate_der(certificate.get()) : std::vector<unsigned char>{};
  HCERTSTORE store = open_root_store(CERT_STORE_READONLY_FLAG);
  if (!store || der.empty()) {
    if (store) {
      CertCloseStore(store, 0);
    }
    return false;
  }
  const bool found = store_contains_der(store, der);
  CertCloseStore(store, 0);
  return found;
}

bool CertificateManager::remove_recorded_trust_entry() {
  const auto metadata = load_metadata(directory_);
  if (!metadata) {
    std::error_code error;
    if (!std::filesystem::exists(directory_, error)) {
      return !error;
    }
    set_error("Appytizer CA ownership metadata is missing; no trust entry or certificate file was removed.");
    return false;
  }
  const auto sha1 = metadata->value("sha1_thumbprint", "");
  const auto sha256 = metadata->value("sha256", "");
  if (sha1.empty() || sha256.empty()) {
    set_error("Appytizer CA ownership metadata is incomplete; no trust entry was removed.");
    return false;
  }
  HCERTSTORE store = open_root_store(CERT_STORE_MAXIMUM_ALLOWED_FLAG);
  if (!store) {
    set_error("Could not open LocalMachine\\Root to remove the Appytizer CA.");
    return false;
  }
  bool success = true;
  PCCERT_CONTEXT current{};
  while ((current = CertEnumCertificatesInStore(store, current)) != nullptr) {
    if (context_digest_hex(current, CALG_SHA1) != sha1 ||
        context_digest_hex(current, CALG_SHA_256) != sha256) {
      continue;
    }
    PCCERT_CONTEXT exact = CertDuplicateCertificateContext(current);
    if (!exact || !CertDeleteCertificateFromStore(exact)) {
      success = false;
    }
    break;
  }
  CertCloseStore(store, 0);
  if (!success) {
    set_error("Could not remove the exact recorded Appytizer CA trust entry.");
  }
  return success;
}

bool CertificateManager::remove() {
  std::scoped_lock lock(mutex_);
  last_error_.clear();
  if (!remove_recorded_trust_entry()) {
    return false;
  }
  std::error_code error;
  std::filesystem::remove_all(directory_, error);
  if (error) {
    set_error("The trust entry was removed, but Appytizer certificate files could not be deleted.");
    return false;
  }
  return true;
}

bool CertificateManager::site_certificate_valid(std::string_view hostname, int renewal_days) const {
  auto ca_certificate = load_certificate(ca_certificate_path(directory_));
  auto certificate = load_certificate(certificate_path(hostname));
  auto key = load_private_key(private_key_path(hostname));
  KeyPtr issuer_key(ca_certificate ? X509_get_pubkey(ca_certificate.get()) : nullptr);
  if (!ca_certificate || !certificate || !key || !issuer_key || EVP_PKEY_bits(key.get()) < 2048 ||
      X509_check_private_key(certificate.get(), key.get()) != 1 ||
      X509_check_issued(ca_certificate.get(), certificate.get()) != X509_V_OK ||
      X509_verify(certificate.get(), issuer_key.get()) != 1 ||
      X509_cmp_current_time(X509_get0_notBefore(certificate.get())) > 0 ||
      expires_within(certificate.get(), renewal_days) || !has_exact_san(certificate.get(), hostname) ||
      !has_exact_server_auth(certificate.get())) {
    return false;
  }
  std::array<char, 256> common_name{};
  const int size = X509_NAME_get_text_by_NID(X509_get_subject_name(certificate.get()), NID_commonName,
                                             common_name.data(), static_cast<int>(common_name.size()));
  return size == static_cast<int>(hostname.size()) && std::string_view(common_name.data(), hostname.size()) == hostname;
}

bool CertificateManager::ensure_site_certificate(std::string_view hostname) {
  std::scoped_lock lock(mutex_);
  last_error_.clear();
  if (!valid_hostname(hostname)) {
    set_error("Refused to issue a certificate for an invalid exact .test hostname.");
    return false;
  }
  if (!ensure_ca_files()) {
    return false;
  }
  if (site_certificate_valid(hostname, kRenewalDays)) {
    return true;
  }
  if (!create_leaf(directory_, hostname, restrict_private_keys_) ||
      !site_certificate_valid(hostname, 0)) {
    set_error("Could not issue a valid certificate for " + std::string(hostname) + ".");
    return false;
  }
  return true;
}

bool CertificateManager::certificate_needs_renewal(std::string_view hostname,
                                                   int threshold_days) const {
  std::scoped_lock lock(mutex_);
  return !site_certificate_valid(hostname, threshold_days);
}

bool CertificateManager::remove_unused_site_certificates(const std::vector<std::string>& hostnames) {
  std::scoped_lock lock(mutex_);
  std::set<std::string, std::less<>> retained(hostnames.begin(), hostnames.end());
  const auto sites = directory_ / L"sites";
  std::error_code error;
  if (!std::filesystem::is_directory(sites, error)) {
    return !error;
  }
  for (const auto& entry : std::filesystem::directory_iterator(sites, error)) {
    if (error) {
      return false;
    }
    const auto filename = entry.path().filename().string();
    const auto marker = filename.find(".test.");
    if (marker == std::string::npos) {
      continue;
    }
    const auto hostname = filename.substr(0, marker + 5);
    if (!retained.contains(hostname)) {
      std::filesystem::remove(entry.path(), error);
      if (error) {
        return false;
      }
    }
  }
  return true;
}

TlsStatus CertificateManager::status() const {
  std::scoped_lock lock(mutex_);
  TlsStatus result;
  const bool files_valid = ca_files_valid();
  result.trusted = files_valid && is_trusted();
  result.ready = files_valid && result.trusted;
  const auto sites = directory_ / L"sites";
  std::error_code error;
  if (std::filesystem::is_directory(sites, error)) {
    for (const auto& entry : std::filesystem::directory_iterator(sites, error)) {
      if (error || entry.path().extension() != L".pem" ||
          !entry.path().filename().wstring().ends_with(L".crt.pem")) {
        continue;
      }
      auto certificate = load_certificate(entry.path());
      if (!certificate) {
        continue;
      }
      ++result.site_certificate_count;
      const auto expiry = expiry_text(certificate.get());
      if (!expiry.empty() && (result.earliest_expiry.empty() || expiry < result.earliest_expiry)) {
        result.earliest_expiry = expiry;
      }
    }
  }
  if (!last_error_.empty()) {
    result.error = last_error_;
  } else if (!files_valid) {
    result.error = "Appytizer TLS is not provisioned. Use Repair certificates as administrator.";
  } else if (!result.trusted) {
    result.error = "The Appytizer root CA is not trusted in LocalMachine\\Root. Use Repair certificates.";
  }
  return result;
}

} // namespace appytizer
