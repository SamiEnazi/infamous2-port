Optional training scenes for "Optimise for this PC" on Linux (tools/pgo_train.py --runs street,plaza).

Each scene is a training save and a recorded walk from the place where that save starts:
  street.pad   the busy street toward the cathedral (84 s of walking, frame-stepped clock)
  plaza.pad    the umbrella plaza (a short walk to the plaza wall, then standing)
  street/gamedata, plaza/gamedata   the training saves (a gamedata folder: dev_hdd0/home/00000001/savedata/...)

The saves are NOT included. Without them these runs are skipped and the training uses the new-game run (as on
Windows). A save is made for one version of the game (its title ID, e.g. BCUS98125 for the US disc): another
version does not list it, and the trainer skips the run with a note.

To use them in setup's training (tools/setup.sh --pgo, the installer's Optimise for this PC): ship the saves and a
runs.txt here whose first line names the runs, e.g. "opening,street,plaza" (without runs.txt: "opening", the new game
only). PORT_PGO_TRAIN=<runs> does the same for one setup run.
