// Lumora development build: Hub, Editor, Player and headless mode in one process.
#include "main/Launch.hpp"

int main(int argc, char** argv) { return lumora::app::runLumora(argc, argv, lumora::app::Flavor::AllInOne); }
