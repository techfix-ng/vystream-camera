<?php
declare(strict_types=1);

header('Content-Type: application/json; charset=utf-8');
header('Cache-Control: no-store');

$configFile = getenv('VYSTRM_CONFIG_FILE') ?: '';
if (!$configFile || !is_file($configFile)) {
    // cPanel/shared-hosting deployments commonly keep config.php beside
    // the public api/ directory. The environment variable remains preferred.
    $candidate = dirname(__DIR__, 2) . '/config/config.php';
    if (is_file($candidate) && is_readable($candidate)) {
        $configFile = $candidate;
    }
}
if (!$configFile || !is_file($configFile) || !is_readable($configFile)) {
    http_response_code(503);
    echo json_encode(['error' => 'service_not_configured']);
    exit;
}
$config = require $configFile;
$pdo = new PDO(
    $config['database']['dsn'],
    $config['database']['username'],
    $config['database']['password'],
    [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION, PDO::ATTR_DEFAULT_FETCH_MODE => PDO::FETCH_ASSOC]
);

function response(array $body, int $status = 200): never {
    http_response_code($status);
    echo json_encode($body, JSON_UNESCAPED_SLASHES);
    exit;
}
function input(): array {
    $data = json_decode(file_get_contents('php://input') ?: '{}', true);
    return is_array($data) ? $data : [];
}
function randomToken(): string {
    return rtrim(strtr(base64_encode(random_bytes(32)), '+/', '-_'), '=');
}
function tokenHash(string $token, array $config): string {
    return hash_hmac('sha256', $token, $config['app']['token_pepper']);
}
function bearerToken(): string {
    $header = $_SERVER['HTTP_AUTHORIZATION'] ?? '';
    return preg_match('/^Bearer\s+(.+)$/i', $header, $match) ? trim($match[1]) : '';
}
function authenticatedUser(PDO $pdo, array $config): array {
    $token = bearerToken();
    if ($token === '') response(['error' => 'authentication_required'], 401);
    $query = $pdo->prepare(
        'SELECT u.* FROM access_tokens t JOIN users u ON u.id=t.user_id
         WHERE t.token_hash=? AND t.revoked_at IS NULL AND t.expires_at>UTC_TIMESTAMP()
         AND u.status="active" LIMIT 1'
    );
    $query->execute([tokenHash($token, $config)]);
    $user = $query->fetch();
    if (!$user) response(['error' => 'invalid_or_expired_token'], 401);
    return $user;
}
function entitlements(PDO $pdo, int $userId): array {
    $query = $pdo->prepare(
        'SELECT p.code,p.max_cameras,p.max_width,p.max_height,s.current_period_ends_at
         FROM subscriptions s JOIN plans p ON p.id=s.plan_id
         WHERE s.user_id=? AND s.status IN ("active","trialing")
         AND s.current_period_ends_at>UTC_TIMESTAMP() AND p.active=1
         ORDER BY p.max_cameras DESC,p.max_width DESC LIMIT 1'
    );
    $query->execute([$userId]);
    $plan = $query->fetch();
    if (!$plan) {
        $plan = $pdo->query(
            'SELECT code,max_cameras,max_width,max_height,NULL current_period_ends_at
             FROM plans WHERE code="free" AND active=1 LIMIT 1'
        )->fetch();
    }
    if (!$plan) {
        $plan = ['code'=>'free','max_cameras'=>1,'max_width'=>1280,
                 'max_height'=>720,'current_period_ends_at'=>null];
    }
    return [
        'plan' => $plan['code'],
        'max_cameras' => (int)$plan['max_cameras'],
        'max_width' => (int)$plan['max_width'],
        'max_height' => (int)$plan['max_height'],
        'valid_until' => $plan['current_period_ends_at']
            ? strtotime($plan['current_period_ends_at']) : 0,
    ];
}
function issueSession(PDO $pdo, array $config, array $user): array {
    $access = randomToken();
    $refresh = randomToken();
    $accessMinutes = max(5, (int)$config['app']['access_token_minutes']);
    $refreshDays = max(1, (int)$config['app']['refresh_token_days']);
    $pdo->beginTransaction();
    try {
        $query = $pdo->prepare(
            'INSERT INTO access_tokens(user_id,token_hash,expires_at)
             VALUES(?,?,DATE_ADD(UTC_TIMESTAMP(),INTERVAL ? MINUTE))'
        );
        $query->execute([$user['id'], tokenHash($access, $config), $accessMinutes]);
        $query = $pdo->prepare(
            'INSERT INTO refresh_tokens(user_id,token_hash,expires_at)
             VALUES(?,?,DATE_ADD(UTC_TIMESTAMP(),INTERVAL ? DAY))'
        );
        $query->execute([$user['id'], tokenHash($refresh, $config), $refreshDays]);
        $pdo->commit();
    } catch (Throwable $error) {
        $pdo->rollBack();
        throw $error;
    }
    return [
        'access_token' => $access,
        'access_token_expires_in' => $accessMinutes * 60,
        'refresh_token' => $refresh,
        'refresh_token_expires_in' => $refreshDays * 86400,
        'user' => ['id'=>(int)$user['id'],'email'=>$user['email'],
                   'display_name'=>$user['display_name']],
        'entitlements' => entitlements($pdo, (int)$user['id']),
    ];
}
function clientIp(): string {
    return $_SERVER['REMOTE_ADDR'] ?? 'unknown';
}

$method = $_SERVER['REQUEST_METHOD'] ?? 'GET';
$path = parse_url($_SERVER['REQUEST_URI'] ?? '/', PHP_URL_PATH) ?: '/';
$path = preg_replace('#^/api/v1#', '', $path) ?: '/';

try {
    if ($method === 'POST' && $path === '/auth/register') {
        $body = input();
        $email = strtolower(trim((string)($body['email'] ?? '')));
        $password = (string)($body['password'] ?? '');
        $displayName = trim((string)($body['display_name'] ?? ''));
        if (!filter_var($email, FILTER_VALIDATE_EMAIL)) {
            response(['error'=>'invalid_email'], 422);
        }
        if (strlen($password) < 10) {
            response(['error'=>'password_too_short','minimum'=>10], 422);
        }
        if ($displayName === '' || strlen($displayName) > 120) {
            response(['error'=>'invalid_display_name'], 422);
        }
        $existing = $pdo->prepare('SELECT id FROM users WHERE email=? LIMIT 1');
        $existing->execute([$email]);
        if ($existing->fetchColumn()) response(['error'=>'email_already_registered'], 409);
        $passwordHash = password_hash($password, PASSWORD_ARGON2ID);
        if ($passwordHash === false) response(['error'=>'password_hash_unavailable'], 500);
        $query = $pdo->prepare(
            'INSERT INTO users(email,password_hash,display_name,status) VALUES(?,?,?,"active")'
        );
        $query->execute([$email,$passwordHash,$displayName]);
        $user = ['id'=>(int)$pdo->lastInsertId(),'email'=>$email,'display_name'=>$displayName];
        response(issueSession($pdo, $config, $user), 201);
    }

    if ($method === 'POST' && $path === '/auth/login') {
        $body = input();
        $email = strtolower(trim((string)($body['email'] ?? '')));
        $password = (string)($body['password'] ?? '');
        if (!filter_var($email, FILTER_VALIDATE_EMAIL) || $password === '') {
            response(['error'=>'invalid_credentials'], 401);
        }

        $emailHash = hash_hmac('sha256', $email, $config['app']['token_pepper']);
        $ipHash = hash_hmac('sha256', clientIp(), $config['app']['token_pepper']);
        $rate = $pdo->prepare(
            'SELECT COUNT(*) FROM login_attempts WHERE email_hash=? AND ip_hash=?
             AND succeeded=0 AND attempted_at>DATE_SUB(UTC_TIMESTAMP(),INTERVAL 15 MINUTE)'
        );
        $rate->execute([$emailHash, $ipHash]);
        if ((int)$rate->fetchColumn() >= 8) response(['error'=>'too_many_attempts'], 429);

        $query = $pdo->prepare('SELECT * FROM users WHERE email=? AND status="active" LIMIT 1');
        $query->execute([$email]);
        $user = $query->fetch();
        $ok = $user && $user['password_hash'] && password_verify($password, $user['password_hash']);
        $attempt = $pdo->prepare(
            'INSERT INTO login_attempts(email_hash,ip_hash,succeeded) VALUES(?,?,?)'
        );
        $attempt->execute([$emailHash, $ipHash, $ok ? 1 : 0]);
        if (!$ok) response(['error'=>'invalid_credentials'], 401);
        if (password_needs_rehash($user['password_hash'], PASSWORD_ARGON2ID)) {
            $rehash = $pdo->prepare('UPDATE users SET password_hash=? WHERE id=?');
            $rehash->execute([password_hash($password, PASSWORD_ARGON2ID), $user['id']]);
        }
        response(issueSession($pdo, $config, $user));
    }

    if ($method === 'POST' && $path === '/auth/refresh') {
        $body = input();
        $refresh = (string)($body['refresh_token'] ?? '');
        if ($refresh === '') response(['error'=>'invalid_refresh_token'], 401);
        $query = $pdo->prepare(
            'SELECT r.*,u.email,u.display_name,u.status FROM refresh_tokens r
             JOIN users u ON u.id=r.user_id
             WHERE r.token_hash=? AND r.revoked_at IS NULL
             AND r.expires_at>UTC_TIMESTAMP() AND u.status="active" LIMIT 1'
        );
        $query->execute([tokenHash($refresh, $config)]);
        $row = $query->fetch();
        if (!$row) response(['error'=>'invalid_refresh_token'], 401);

        // The refresh-token row has its own primary key; it is not the
        // user's id. Passing the row directly to issueSession() caused a
        // foreign-key failure (HTTP 500) during refresh, then the old token
        // was already revoked and every retry became HTTP 401. Issue the
        // replacement session for the actual user first, and revoke the old
        // token only after the replacement was created successfully.
        $sessionUser = [
            'id' => (int)$row['user_id'],
            'email' => $row['email'],
            'display_name' => $row['display_name'],
        ];
        try {
            $session = issueSession($pdo, $config, $sessionUser);
            $pdo->prepare('UPDATE refresh_tokens SET revoked_at=UTC_TIMESTAMP() WHERE id=?')
                ->execute([(int)$row['id']]);
            response($session);
        } catch (Throwable $error) {
            error_log('VYSTREAM refresh session failed: ' . $error->getMessage());
            response(['error'=>'refresh_session_failed'], 500);
        }
    }

    if ($method === 'POST' && $path === '/auth/logout') {
        $token = bearerToken();
        if ($token !== '') {
            $pdo->prepare('UPDATE access_tokens SET revoked_at=UTC_TIMESTAMP() WHERE token_hash=?')
                ->execute([tokenHash($token, $config)]);
        }
        response(['ok'=>true]);
    }

    if ($method === 'POST' && $path === '/account/entitlements') {
        $user = authenticatedUser($pdo, $config);
        $value = entitlements($pdo, (int)$user['id']);
        $audit = $pdo->prepare(
            'INSERT INTO entitlement_audit
             (user_id,plan_code,max_cameras,max_width,max_height,source)
             VALUES(?,?,?,?,?,"obs-plugin")'
        );
        $audit->execute([$user['id'],$value['plan'],$value['max_cameras'],
                         $value['max_width'],$value['max_height']]);
        response(['entitlements'=>$value]);
    }

    if ($method === 'POST' && $path === '/auth/forgot-password') {
        $email = strtolower(trim((string)(input()['email'] ?? '')));
        $query = $pdo->prepare('SELECT id FROM users WHERE email=? AND status="active" LIMIT 1');
        $query->execute([$email]);
        if ($id = $query->fetchColumn()) {
            $token = randomToken();
            $pdo->beginTransaction();
            try {
                $pdo->prepare(
                    'UPDATE password_reset_tokens SET used_at=UTC_TIMESTAMP()
                     WHERE user_id=? AND used_at IS NULL'
                )->execute([$id]);
                $pdo->prepare(
                    'INSERT INTO password_reset_tokens(user_id,token_hash,expires_at)
                     VALUES(?,?,DATE_ADD(UTC_TIMESTAMP(),INTERVAL 30 MINUTE))'
                )->execute([$id, tokenHash($token, $config)]);
                $resetUrl = rtrim($config['app']['base_url'], '/').'/reset-password?token='.urlencode($token);
                $payload = json_encode(['reset_url'=>$resetUrl,'expires_minutes'=>30], JSON_UNESCAPED_SLASHES);
                $pdo->prepare(
                    'INSERT INTO email_queue(recipient,template,payload_json) VALUES(?,"password-reset",?)'
                )->execute([$email,$payload]);
                $pdo->commit();
            } catch (Throwable $error) {
                $pdo->rollBack();
                throw $error;
            }
        }
        response(['ok'=>true,'message'=>'If the account exists, reset instructions will be sent.']);
    }

    if ($method === 'POST' && $path === '/auth/reset-password') {
        $body = input();
        $token = (string)($body['token'] ?? '');
        $password = (string)($body['password'] ?? '');
        if ($token === '') response(['error'=>'invalid_reset_token'], 422);
        if (strlen($password) < 10) {
            response(['error'=>'password_too_short','minimum'=>10], 422);
        }
        $query = $pdo->prepare(
            'SELECT id,user_id FROM password_reset_tokens
             WHERE token_hash=? AND used_at IS NULL AND expires_at>UTC_TIMESTAMP() LIMIT 1'
        );
        $query->execute([tokenHash($token, $config)]);
        $reset = $query->fetch();
        if (!$reset) response(['error'=>'invalid_or_expired_reset_token'], 422);
        $passwordHash = password_hash($password, PASSWORD_ARGON2ID);
        if ($passwordHash === false) response(['error'=>'password_hash_unavailable'], 500);
        $pdo->beginTransaction();
        try {
            $pdo->prepare('UPDATE users SET password_hash=? WHERE id=?')
                ->execute([$passwordHash,$reset['user_id']]);
            $pdo->prepare('UPDATE password_reset_tokens SET used_at=UTC_TIMESTAMP() WHERE id=?')
                ->execute([$reset['id']]);
            $pdo->prepare('UPDATE access_tokens SET revoked_at=UTC_TIMESTAMP() WHERE user_id=? AND revoked_at IS NULL')
                ->execute([$reset['user_id']]);
            $pdo->prepare('UPDATE refresh_tokens SET revoked_at=UTC_TIMESTAMP() WHERE user_id=? AND revoked_at IS NULL')
                ->execute([$reset['user_id']]);
            $pdo->commit();
        } catch (Throwable $error) {
            $pdo->rollBack();
            throw $error;
        }
        response(['ok'=>true]);
    }

    if ($method === 'GET' && $path === '/account/profile') {
        $user = authenticatedUser($pdo, $config);
        response(['user'=>[
            'id'=>(int)$user['id'],
            'email'=>$user['email'],
            'display_name'=>$user['display_name'],
            'email_verified'=>(bool)$user['email_verified_at'],
        ],'entitlements'=>entitlements($pdo,(int)$user['id'])]);
    }

    if ($method === 'GET' && ($path === '/oauth/google/start' || $path === '/oauth/apple/start')) {
        response(['error'=>'oauth_provider_not_configured'], 501);
    }

    if ($method === 'GET' && $path === '/health') {
        response(['ok'=>true,'service'=>'vystrm-auth']);
    }

    response(['error'=>'not_found'], 404);
} catch (Throwable $error) {
    error_log('VYSTRM API: '.$error->getMessage());
    response(['error'=>'server_error'], 500);
}
