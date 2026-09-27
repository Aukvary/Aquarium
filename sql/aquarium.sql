SELECT 'CREATE DATABASE aquarium'
WHERE NOT EXISTS (SELECT 1 FROM pg_database WHERE datname = 'aquarium')\gexec
