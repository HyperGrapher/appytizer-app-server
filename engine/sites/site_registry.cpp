#include "engine/sites/site_registry.hpp"

#include "common/config.hpp"
#include "common/constants.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#include <vector>

namespace appytizer {
namespace {
struct ScannedSite {
  std::string name;
  std::string hostname;
  std::string path;
  std::string type;
  std::string error;
};

std::string lowercase_ascii(std::string value) {
  std::ranges::transform(value, value.begin(), [](unsigned char character) {
    return static_cast<char>(std::tolower(character));
  });
  return value;
}

bool execute(sqlite3* database, const char* sql) {
  return sqlite3_exec(database, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
}

std::string quoted_path(const std::filesystem::path& path) {
  return "\"" + path.generic_string() + "\"";
}
} // namespace

SiteRegistry::SiteRegistry() {
  const auto path = ConfigStore::default_path().parent_path() / L"app.db";
  std::error_code error;
  std::filesystem::create_directories(path.parent_path(), error);
  if (sqlite3_open_v2(path.string().c_str(), &db_,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                      nullptr) != SQLITE_OK) {
    return;
  }
  execute(db_, "DROP TABLE IF EXISTS sites;");
  execute(db_,
          "CREATE TABLE sites("
          "id INTEGER PRIMARY KEY,"
          "folder_name TEXT NOT NULL,"
          "hostname TEXT NOT NULL,"
          "path TEXT NOT NULL,"
          "detected_type TEXT NOT NULL,"
          "validation_error TEXT NOT NULL DEFAULT '');");
}

SiteRegistry::~SiteRegistry() {
  if (db_) {
    sqlite3_close(db_);
  }
}

std::string SiteRegistry::validate_dns_label(std::string_view label) {
  if (label.empty()) {
    return "Folder name is empty.";
  }
  if (label.size() > 63) {
    return "Folder name exceeds the 63-character DNS label limit.";
  }
  if (label.front() == '-' || label.back() == '-') {
    return "DNS labels cannot start or end with a hyphen.";
  }
  if (!std::ranges::all_of(label, [](unsigned char character) {
        return (character >= 'a' && character <= 'z') ||
               (character >= 'A' && character <= 'Z') ||
               (character >= '0' && character <= '9') || character == '-';
      })) {
    return "Folder name must contain only ASCII letters, digits, and hyphens.";
  }
  return {};
}

std::map<std::string, std::string, std::less<>>
SiteRegistry::validate_dns_labels(const std::vector<std::string>& labels) {
  std::map<std::string, std::string, std::less<>> errors;
  std::map<std::string, std::vector<std::string>, std::less<>> normalized_labels;
  for (const auto& label : labels) {
    const auto error = validate_dns_label(label);
    if (!error.empty()) {
      errors[label] = error;
    } else {
      normalized_labels[lowercase_ascii(label)].push_back(label);
    }
  }
  for (const auto& [normalized, originals] : normalized_labels) {
    if (originals.size() < 2) {
      continue;
    }
    for (const auto& original : originals) {
      errors[original] = "Case-insensitive hostname collision for " + normalized +
                         std::string(kSiteSuffix) + ".";
    }
  }
  return errors;
}

bool SiteRegistry::rescan(const std::filesystem::path& root) {
  if (!db_) {
    return false;
  }

  std::vector<ScannedSite> scanned;
  std::error_code error;
  if (std::filesystem::is_directory(root, error)) {
    for (const auto& entry : std::filesystem::directory_iterator(root, error)) {
      if (error) {
        return false;
      }
      if (!entry.is_directory(error)) {
        if (error) {
          return false;
        }
        continue;
      }
      const auto name = entry.path().filename().string();
      const auto normalized = lowercase_ascii(name);
      scanned.push_back({name,
                         normalized + std::string(kSiteSuffix),
                         entry.path().string(),
                         std::filesystem::exists(entry.path() / "index.php", error) ? "PHP" : "Static",
                         validate_dns_label(name)});
      if (error) {
        return false;
      }
    }
  } else if (error) {
    return false;
  }

  std::vector<std::string> names;
  names.reserve(scanned.size());
  for (const auto& site : scanned) {
    names.push_back(site.name);
  }
  const auto validation_errors = validate_dns_labels(names);
  for (auto& site : scanned) {
    if (const auto found = validation_errors.find(site.name); found != validation_errors.end()) {
      site.error = found->second;
    }
  }

  if (!execute(db_, "BEGIN IMMEDIATE; DELETE FROM sites;")) {
    return false;
  }
  sqlite3_stmt* statement{};
  if (sqlite3_prepare_v2(db_,
                         "INSERT INTO sites(folder_name,hostname,path,detected_type,validation_error) "
                         "VALUES(?,?,?,?,?);",
                         -1, &statement, nullptr) != SQLITE_OK) {
    execute(db_, "ROLLBACK;");
    return false;
  }
  bool inserted = true;
  for (const auto& site : scanned) {
    sqlite3_reset(statement);
    sqlite3_clear_bindings(statement);
    sqlite3_bind_text(statement, 1, site.name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, site.hostname.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 3, site.path.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 4, site.type.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 5, site.error.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(statement) != SQLITE_DONE) {
      inserted = false;
      break;
    }
  }
  sqlite3_finalize(statement);
  if (!inserted || !execute(db_, "COMMIT;")) {
    execute(db_, "ROLLBACK;");
    return false;
  }
  return true;
}

nlohmann::json SiteRegistry::list() const {
  nlohmann::json result = nlohmann::json::array();
  if (!db_) {
    return result;
  }
  sqlite3_stmt* statement{};
  if (sqlite3_prepare_v2(db_,
                         "SELECT folder_name,hostname,path,detected_type,validation_error "
                         "FROM sites ORDER BY folder_name COLLATE NOCASE, folder_name;",
                         -1, &statement, nullptr) != SQLITE_OK) {
    return result;
  }
  while (sqlite3_step(statement) == SQLITE_ROW) {
    const auto error = std::string(reinterpret_cast<const char*>(sqlite3_column_text(statement, 4)));
    result.push_back({{"name", reinterpret_cast<const char*>(sqlite3_column_text(statement, 0))},
                      {"hostname", reinterpret_cast<const char*>(sqlite3_column_text(statement, 1))},
                      {"path", reinterpret_cast<const char*>(sqlite3_column_text(statement, 2))},
                      {"type", reinterpret_cast<const char*>(sqlite3_column_text(statement, 3))},
                      {"valid", error.empty()},
                      {"error", error}});
  }
  sqlite3_finalize(statement);
  return result;
}

bool SiteRegistry::write_nginx_configs(const std::filesystem::path& output, bool https_enabled,
                                       const std::filesystem::path& certificates_directory,
                                       std::uint16_t php_port,
                                       const std::filesystem::path& fastcgi_params) const {
  std::error_code error;
  std::filesystem::create_directories(output, error);
  if (error) {
    return false;
  }
  const auto params = fastcgi_params.empty() ? std::string("fastcgi_params") : quoted_path(fastcgi_params);
  for (const auto& site : list()) {
    if (!site.value("valid", false)) {
      continue;
    }
    const auto hostname = site.at("hostname").get<std::string>();
    std::ofstream file(output / (hostname + ".conf"), std::ios::trunc);
    if (!file) {
      return false;
    }
    if (https_enabled) {
      file << "server {\n"
              "  listen 80;\n"
              "  server_name " << hostname << ";\n"
              "  return 308 https://$host$request_uri;\n"
              "}\n\n"
              "server {\n"
              "  listen 443 ssl;\n"
              "  server_name " << hostname << ";\n"
              "  ssl_certificate "
           << quoted_path(certificates_directory / L"sites" / (hostname + ".crt.pem")) << ";\n"
              "  ssl_certificate_key "
           << quoted_path(certificates_directory / L"sites" / (hostname + ".key.pem")) << ";\n";
    } else {
      file << "server {\n"
              "  listen 80;\n"
              "  server_name " << hostname << ";\n";
    }
    const auto root = std::filesystem::path(site.at("path").get<std::string>());
    file << "  root " << quoted_path(root) << ";\n"
            "  index index.php index.html;\n"
            "  location / { try_files $uri $uri/ /index.php?$query_string; }\n"
            "  location ~ \\.php$ {\n"
            "    include " << params << ";\n"
            "    fastcgi_pass 127.0.0.1:" << php_port << ";\n"
            "    fastcgi_param SCRIPT_FILENAME $document_root$fastcgi_script_name;\n"
            "  }\n"
            "}\n";
    if (!file) {
      return false;
    }
  }
  return true;
}

bool SiteRegistry::write_nginx_root_config(const std::filesystem::path& output,
                                           const std::filesystem::path& nginx_root,
                                           bool https_enabled) const {
  std::error_code error;
  const auto runtime = output.parent_path().parent_path();
  std::filesystem::create_directories(output.parent_path(), error);
  std::filesystem::create_directories(runtime / L"logs", error);
  for (const auto* name : {L"client_body_temp", L"proxy_temp", L"fastcgi_temp", L"uwsgi_temp", L"scgi_temp"}) {
    std::filesystem::create_directories(runtime / L"temp" / name, error);
  }
  if (error) {
    return false;
  }
  std::ofstream file(output, std::ios::trunc);
  if (!file) {
    return false;
  }
  file << "worker_processes 1;\n"
          "error_log logs/error.log notice;\n"
          "pid logs/nginx.pid;\n"
          "events { worker_connections 1024; }\n"
          "http {\n"
          "  include " << quoted_path(nginx_root / L"conf" / L"mime.types") << ";\n"
          "  default_type application/octet-stream;\n"
          "  sendfile on;\n"
          "  server_tokens off;\n"
          "  server { listen 80 default_server; server_name _; return 404; }\n";
  if (https_enabled) {
    file << "  server { listen 443 ssl default_server; server_name _; ssl_reject_handshake on; }\n";
  }
  file << "  include ../../sites/*.conf;\n"
          "}\n";
  return static_cast<bool>(file);
}

} // namespace appytizer
