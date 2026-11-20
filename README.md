# Aquarium 🐠

Высокопроизводительный асинхронный серверный бэкенд трекера задач для команды, разработанный на **C++20** с использованием фреймворка **userver** от Яндекса, протокола **gRPC** и базы данных **PostgreSQL 16**.

---

## Быстрый старт с Docker (Docker Compose)

Для запуска всего сервиса и базы данных на любой машине достаточно иметь установленный **Docker** и **Docker Compose**.

### 1. Клонирование репозитория

```bash
git clone <URL_РЕПОЗИТОРИЯ>
cd Aquarium
```

### 2. (Опционально) Настройка окружения

Если вы хотите изменить стандартные порты или пароли, создайте файл `.env` на основе примера:

```bash
cp .env.example .env
```

По умолчанию сервис работает из коробки со стандартными параметрами (без необходимости создавать `.env`).

### 3. Запуск контейнеров

Запустите контейнеры с автоматической сборкой:

```bash
docker compose up --build
```

Или в фоновом режиме (detached mode):

```bash
docker compose up -d --build
```

### Что происходит при запуске:
1. **Контейнер базы данных (`aquarium-db`)**:
   - Запускается PostgreSQL 16.
   - Автоматически применяются миграции и схема БД в правильном порядке:
     - `sql/users.sql` (схема `user_schema`, таблица токенов и пользователей).
     - `sql/Tasks.sql` (схема `task_schema`, задачи, исполнители, триггеры обновления и вью `task_cache_view`).
   - Проверяется `healthcheck` (`pg_isready`).
2. **Контейнер приложения (`aquarium-app`)**:
   - Собирается сервис с использованием готового окружения `userver` (быстрая компиляция без ожидания сборки фреймворка).
   - Дожидается готовности базы данных (`depends_on: condition: service_healthy`).
   - Подключается к базе, инициализирует in-memory кэши (`auth-pg-cache` и `tasks-pg-cache`).
   - Открывает порты для приёма запросов.

---

## Сетевые порты

| Сервис | Порт по умолчанию | Описание |
|---|---|---|
| **gRPC API** | `8081` | Основной gRPC интерфейс (`UserStoreService`, `TaskStoreService`, `UtilsService`) |
| **HTTP Listener** | `8080` | Системный HTTP порт userver (мониторинг / метрики / healthcheck) |
| **PostgreSQL** | `5432` | Доступ к базе данных (для локальных инструментов DBeaver, psql и др.) |

---

## Полезные команды Docker

- **Просмотр логов приложения**:
  ```bash
  docker compose logs -f app
  ```

- **Просмотр логов базы данных**:
  ```bash
  docker compose logs -f db
  ```

- **Остановка контейнеров**:
  ```bash
  docker compose down
  ```

- **Остановка и полный сброс данных БД (очистка volume)**:
  ```bash
  docker compose down -v
  ```

- **Запуск только базы данных** (для локальной разработки и отладки C++ кода на хосте):
  ```bash
  docker compose up -d db
  ./build.sh
  ```

---

## Переменные окружения (.env)

| Переменная | По умолчанию | Назначение |
|---|---|---|
| `POSTGRES_USER` | `admin` | Пользователь PostgreSQL |
| `POSTGRES_PASSWORD` | `password` | Пароль пользователя PostgreSQL |
| `POSTGRES_DB` | `aquarium` | Имя базы данных |
| `POSTGRES_PORT` | `5432` | Внешний порт PostgreSQL на хосте |
| `GRPC_PORT` | `8081` | Внешний порт gRPC сервиса |
| `HTTP_PORT` | `8080` | Внешний системный HTTP-порт |
| `INTERNAL_KEY` | `zxc` | Секретный ключ для внутренних межсервисных вызовов |
| `INVITE_KEY` | `secret_invite_key_value` | Секретный ключ инвайта для регистрации пользователей через RPC `AddUser` |

---

## Проверка работоспособности

### Проверка gRPC Healthcheck через grpcurl

```bash
grpcurl -plaintext -H "authorization: Bearer zxc" localhost:8081 aquarium.api.UtilsService/Health
```

### Добавление первого пользователя через RPC `AddUser`

```bash
grpcurl -plaintext -H "invite-key: Invite secret_invite_key_value" -d '{"name": "Admin"}' localhost:8081 aquarium.api.UserStoreService/AddUser
```
