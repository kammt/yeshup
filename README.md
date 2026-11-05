# yeshup

nohup's counterpart.

`nohup` makes sure a command survives a hangup. `yeshup` makes sure it doesn't:

- **SIGHUP is always passed through.** The command starts with SIGHUP at its
  default disposition and unblocked, even if `yeshup` itself was started with
  SIGHUP ignored (for example under `nohup`). Any SIGHUP sent to `yeshup` is
  forwarded to the command.
- **A hangup is coming.** At a random time between 2 and 10 minutes after
  start, `yeshup` sends SIGHUP to the command itself.

```
$ yeshup sleep 3600
Hangup
$ echo $?
129
```

## Build and install

Works on Linux and macOS. Needs a C compiler and `make`.

```
make
make test
sudo make install          # installs to /usr/local/bin (set PREFIX to change)
```

## Usage

```
yeshup COMMAND [ARG]...
yeshup --help | --version
```

Exit status is the command's exit status. If the command is killed by a
signal, `yeshup` dies of the same signal, so the shell reports it the same way.
Otherwise, as with `nohup`:

| Status | Meaning                                       |
|--------|-----------------------------------------------|
| 125    | `yeshup` itself failed                        |
| 126    | the command was found but could not be run    |
| 127    | the command could not be found                |

`yeshup` also forwards SIGTERM to the command. SIGINT and SIGQUIT are left to
the terminal, which already delivers them to the command.

Unlike `nohup`, `yeshup` leaves stdout and stderr alone.
