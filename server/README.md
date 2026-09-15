# VYSTREAM authentication service

This folder contains the PHP 8.2+ JSON API and MySQL/MariaDB schema used by
the VYSTREAM OBS Plugin.

## Deploy

1. Import `database/schema.sql` into MySQL 8 or MariaDB 10.6+.
2. Deploy the contents of `api/` to the document root serving
   `https://vystream.techfixng.com/api/v1`.
3. Copy `api/config.example.php` to a private directory outside the public web root.
4. Replace the database password and token pepper. Generate the pepper with
   `php -r "echo bin2hex(random_bytes(32)), PHP_EOL;"`.
5. Set the web server environment variable `VYSTRM_CONFIG_FILE` to the absolute
   path of that private configuration file.
6. Give the API database user SELECT, INSERT, UPDATE and DELETE permission only.
7. Confirm `GET https://vystream.techfixng.com/api/v1/health` returns
   `{"ok":true,"service":"vystrm-auth"}`.

Apache routing and API security headers are included in `api/.htaccess`.
Nginx deployments should route every missing path under `/api/v1/` to
`index.php`.

## Authentication endpoints

- `POST /api/v1/auth/register`
- `POST /api/v1/auth/login`
- `POST /api/v1/auth/refresh`
- `POST /api/v1/auth/logout`
- `POST /api/v1/auth/forgot-password`
- `POST /api/v1/auth/reset-password`
- `GET /api/v1/account/profile`
- `POST /api/v1/account/entitlements`
- `GET /api/v1/oauth/google/start`
- `GET /api/v1/oauth/apple/start`
- `GET /api/v1/health`

Registration requires `email`, `display_name`, and a password of at least
10 characters. Login and registration return an opaque 15-minute access token,
a rotating 30-day refresh token, the user profile and current entitlements.

Password-reset requests place a message in `email_queue`; a separate cPanel
cron/mailer must deliver queued messages. The reset endpoint consumes the token
and revokes every existing session for the account.

## Entitlements

The free plan is the safe fallback and is seeded as one camera at 1280×720.
Paid limits come from an active subscription's plan record. Initial placeholder
plans are Creator (three cameras, 1920×1080) and Studio (eight cameras,
3840×2160); pricing remains NULL until the product prices are approved.

Google and Apple browser endpoints are deliberately disabled with HTTP 501 until
their provider credentials and callbacks are configured. The buttons must not be
presented as working production login methods before that work is complete.

Remembered plugin refresh tokens are stored in Windows Credential Manager and
are never written to OBS configuration files.
