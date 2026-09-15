CREATE DATABASE IF NOT EXISTS vystrm CHARACTER SET utf8mb4 COLLATE utf8mb4_unicode_ci;
USE vystrm;

CREATE TABLE users (
  id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  email VARCHAR(254) NOT NULL UNIQUE,
  password_hash VARCHAR(255) NULL,
  display_name VARCHAR(120) NOT NULL DEFAULT '',
  email_verified_at DATETIME NULL,
  status ENUM('active','suspended','deleted') NOT NULL DEFAULT 'active',
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  updated_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP
) ENGINE=InnoDB;

CREATE TABLE plans (
  id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  code VARCHAR(40) NOT NULL UNIQUE,
  name VARCHAR(80) NOT NULL,
  max_cameras SMALLINT UNSIGNED NOT NULL DEFAULT 1,
  max_width SMALLINT UNSIGNED NOT NULL DEFAULT 1280,
  max_height SMALLINT UNSIGNED NOT NULL DEFAULT 720,
  monthly_price_ngn DECIMAL(12,2) NULL,
  yearly_price_ngn DECIMAL(12,2) NULL,
  active TINYINT(1) NOT NULL DEFAULT 1,
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB;

INSERT INTO plans (code,name,max_cameras,max_width,max_height,monthly_price_ngn,yearly_price_ngn)
VALUES
 ('free','Free',1,1280,720,0,0),
 ('creator','Creator',3,1920,1080,NULL,NULL),
 ('studio','Studio',8,3840,2160,NULL,NULL)
ON DUPLICATE KEY UPDATE name=VALUES(name);

CREATE TABLE subscriptions (
  id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  user_id BIGINT UNSIGNED NOT NULL,
  plan_id BIGINT UNSIGNED NOT NULL,
  provider VARCHAR(40) NULL,
  provider_reference VARCHAR(191) NULL,
  status ENUM('trialing','active','past_due','cancelled','expired') NOT NULL,
  starts_at DATETIME NOT NULL,
  current_period_ends_at DATETIME NOT NULL,
  cancelled_at DATETIME NULL,
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  INDEX idx_subscription_user_status (user_id,status,current_period_ends_at),
  UNIQUE KEY uq_provider_reference (provider,provider_reference),
  CONSTRAINT fk_subscription_user FOREIGN KEY (user_id) REFERENCES users(id),
  CONSTRAINT fk_subscription_plan FOREIGN KEY (plan_id) REFERENCES plans(id)
) ENGINE=InnoDB;

CREATE TABLE oauth_identities (
  id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  user_id BIGINT UNSIGNED NOT NULL,
  provider ENUM('google','apple') NOT NULL,
  provider_subject VARCHAR(191) NOT NULL,
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uq_oauth_identity (provider,provider_subject),
  CONSTRAINT fk_oauth_user FOREIGN KEY (user_id) REFERENCES users(id)
) ENGINE=InnoDB;

CREATE TABLE plugin_installations (
  id CHAR(36) PRIMARY KEY,
  user_id BIGINT UNSIGNED NOT NULL,
  installation_hash CHAR(64) NOT NULL,
  computer_name VARCHAR(120) NOT NULL DEFAULT '',
  last_ip VARBINARY(16) NULL,
  app_version VARCHAR(32) NOT NULL,
  last_seen_at DATETIME NOT NULL,
  revoked_at DATETIME NULL,
  UNIQUE KEY uq_user_installation (user_id,installation_hash),
  CONSTRAINT fk_installation_user FOREIGN KEY (user_id) REFERENCES users(id)
) ENGINE=InnoDB;

CREATE TABLE access_tokens (
  id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  user_id BIGINT UNSIGNED NOT NULL,
  installation_id CHAR(36) NULL,
  token_hash CHAR(64) NOT NULL UNIQUE,
  expires_at DATETIME NOT NULL,
  revoked_at DATETIME NULL,
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  INDEX idx_access_expiry (expires_at),
  CONSTRAINT fk_access_user FOREIGN KEY (user_id) REFERENCES users(id),
  CONSTRAINT fk_access_installation FOREIGN KEY (installation_id) REFERENCES plugin_installations(id)
) ENGINE=InnoDB;

CREATE TABLE refresh_tokens (
  id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  user_id BIGINT UNSIGNED NOT NULL,
  installation_id CHAR(36) NULL,
  token_hash CHAR(64) NOT NULL UNIQUE,
  expires_at DATETIME NOT NULL,
  revoked_at DATETIME NULL,
  replaced_by_id BIGINT UNSIGNED NULL,
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  INDEX idx_refresh_expiry (expires_at),
  CONSTRAINT fk_refresh_user FOREIGN KEY (user_id) REFERENCES users(id),
  CONSTRAINT fk_refresh_installation FOREIGN KEY (installation_id) REFERENCES plugin_installations(id)
) ENGINE=InnoDB;

CREATE TABLE password_reset_tokens (
  id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  user_id BIGINT UNSIGNED NOT NULL,
  token_hash CHAR(64) NOT NULL UNIQUE,
  expires_at DATETIME NOT NULL,
  used_at DATETIME NULL,
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  CONSTRAINT fk_reset_user FOREIGN KEY (user_id) REFERENCES users(id)
) ENGINE=InnoDB;

CREATE TABLE entitlement_audit (
  id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  user_id BIGINT UNSIGNED NOT NULL,
  plan_code VARCHAR(40) NOT NULL,
  max_cameras SMALLINT UNSIGNED NOT NULL,
  max_width SMALLINT UNSIGNED NOT NULL,
  max_height SMALLINT UNSIGNED NOT NULL,
  source VARCHAR(60) NOT NULL,
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  INDEX idx_entitlement_user_created (user_id,created_at),
  CONSTRAINT fk_entitlement_user FOREIGN KEY (user_id) REFERENCES users(id)
) ENGINE=InnoDB;

CREATE TABLE login_attempts (
  id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  email_hash CHAR(64) NOT NULL,
  ip_hash CHAR(64) NOT NULL,
  attempted_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  succeeded TINYINT(1) NOT NULL DEFAULT 0,
  INDEX idx_login_rate (email_hash,ip_hash,attempted_at)
) ENGINE=InnoDB;


CREATE TABLE email_queue (
  id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  recipient VARCHAR(254) NOT NULL,
  template VARCHAR(80) NOT NULL,
  payload_json JSON NOT NULL,
  available_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  sent_at DATETIME NULL,
  failed_at DATETIME NULL,
  attempts SMALLINT UNSIGNED NOT NULL DEFAULT 0,
  last_error VARCHAR(500) NULL,
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  INDEX idx_email_delivery (sent_at,failed_at,available_at)
) ENGINE=InnoDB;

CREATE TABLE subscription_events (
  id BIGINT UNSIGNED AUTO_INCREMENT PRIMARY KEY,
  provider VARCHAR(40) NOT NULL,
  provider_event_id VARCHAR(191) NOT NULL,
  event_type VARCHAR(100) NOT NULL,
  payload_json JSON NOT NULL,
  processed_at DATETIME NULL,
  created_at DATETIME NOT NULL DEFAULT CURRENT_TIMESTAMP,
  UNIQUE KEY uq_subscription_event (provider,provider_event_id)
) ENGINE=InnoDB;
