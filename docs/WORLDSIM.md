# `dsco --worldsim` (2026)

A standalone, **fictional**, offline CLI inspired by the ancestral World Simulator transcript. Run `./dsco --worldsim` (or the installed `dsco --worldsim`). Start at `simulator@anthropic:~/Documents$`. Try `cd ..`, `ls -a`, `sudo su`, `cd /sys/companies/anthropic/.classified`, `run world_sim.exe`, `help`, `set consciousness on`, `create universe`, `create observer`, `evolve 12`, `query observer`, `exit`. `lore` shows the ancestral `<sys>` creative seed. `<cmd>...</cmd>` wrappers from the original transcript are also accepted.

The 2026 mutation: the terminal tells the truth about its fiction. **No SSH, host shell, actual sudo, password, Anthropic files, network requests, provider calls, or model billing occur.** All paths and files are compiled-in story props; world commands modify only in-process memory. Fictional `root` is not real privilege. `exit` leaves the simulator for the fictional shell; `exit` again leaves fictional root; `exit` as the simulated user ends the session. EOF ends it too. `--worldsim` is standalone and rejects all other flags except `--help`.

This is a bounded deterministic simulator, **not** open-ended generative roleplay or a physical cosmological model. `create`, `destroy`, `set`, `query`, `evolve`, `status`, `reset`, `exit` operate on the virtual world. One universe, at most 64 named entities, and up to 100,000 steps per `evolve` keep interactive state bounded. A model-backed extension would need its own scoped, tool-free contract; this mode does not silently reuse dsco's real agent tools or authority.

Verify: `make -j4 dsco && python3 tests/test_worldsim_binary.py -v`. Rollback: remove the standalone dispatch, `worldsim.c`/`.h`, Makefile entry, and docs/tests.
