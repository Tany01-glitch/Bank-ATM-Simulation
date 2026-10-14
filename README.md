# NovaBank — Bank ATM Simulation System

Recovered from the C++ source saved in your previous ChatGPT conversations and packaged with a new responsive web frontend. This is an educational demo, not a real banking application.

## Features
- Create a demo account and sign in
- View account balance and profile
- Deposit and withdraw funds
- Transfer funds to another demo account
- Transaction history
- Admin account overview
- C++ HTTP backend with SQLite persistence

## Project structure

- `backend/main.cpp` — recovered C++ backend
- `frontend/index.html` — web interface
- `frontend/style.css` — responsive styling
- `frontend/app.js` — frontend/API integration

The backend source expects the header dependencies `httplib.h`, `json.hpp`, and SQLite development headers/library. Those third-party dependency files were not present in the recovered saved source, so they are not represented as original files in this ZIP.

## Run it

1. Install a C++ compiler (for example, MinGW-w64), CMake or suitable build tools, and SQLite development files.
2. Put `httplib.h` (cpp-httplib) and `json.hpp` (nlohmann/json) in `backend/`.
3. Compile from the `backend` directory, linking SQLite. Example for a MinGW setup where SQLite is installed and linkable:

   `g++ main.cpp -o bank_atm.exe -std=c++17 -lsqlite3 -lws2_32`

4. Run the executable **from the project root** so the source's `../frontend` static path may need to be adjusted. Recommended: change `svr.set_mount_point("/", "../frontend");` in `backend/main.cpp` to `svr.set_mount_point("/", "frontend");` and launch `bank_atm.exe` from the project root, or launch from `backend/` with the original relative path.
5. Open `http://localhost:8080`.

Admin demo login: account `ADMIN`, PIN `9999`.

Create a normal account through **Create account** first to test deposit, withdrawal and transfers. The database `bank.db` is created automatically the first time the backend runs.

## Important recovery note
This ZIP includes the saved C++ source found in your previous files and a replacement frontend to make the recovered backend usable. It is not the exact original ZIP archive, which was not available in the saved files. The code uses demo-grade PIN hashing and is not suitable for real financial use.
