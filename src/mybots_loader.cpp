// AzerothCore loader. Folder name must be `mod-mybots` so this symbol is found.
void AddMyBotsCommandScripts();
void AddMyBotsWorldScripts();
void AddMyBotsServerScripts();

void Addmod_mybotsScripts()
{
    AddMyBotsCommandScripts();
    AddMyBotsWorldScripts();
    AddMyBotsServerScripts();
}
