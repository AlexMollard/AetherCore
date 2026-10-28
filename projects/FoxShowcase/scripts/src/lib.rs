mod interactions;
mod player;
mod wildlife;

use interactions::RuinExperiment;
use player::ExplorerController;
use wildlife::WildlifeWalker;

aethercore::export_gameplay!(ExplorerController, WildlifeWalker, RuinExperiment);
