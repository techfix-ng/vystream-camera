<?php
declare(strict_types=1);

return [
    'database' => [
        'dsn' => 'mysql:host=localhost;dbname=vystrm;charset=utf8mb4',
        'username' => 'vystrm_api',
        'password' => 'CHANGE_ME',
    ],
    'app' => [
        'base_url' => 'https://vystream.techfixng.com',
        'token_pepper' => 'GENERATE_A_LONG_RANDOM_SECRET',
        'access_token_minutes' => 15,
        'refresh_token_days' => 30,
    ],
    'oauth' => [
        'google_client_id' => '',
        'google_client_secret' => '',
        'apple_client_id' => '',
        'apple_team_id' => '',
        'apple_key_id' => '',
        'apple_private_key_path' => '',
    ],
];
