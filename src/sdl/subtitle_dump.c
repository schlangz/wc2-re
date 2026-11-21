#include "game.h"

#include <direct.h>
#include <stdarg.h>

/* Diagnostic-only tool: dump every cutscene TEXT resource chunk found in a
 * campaign's story scenes to numbered files, one subfolder per campaign,
 * then exit. Not part of the retail game -- see docs/SDL2.md.
 *
 * RunCampaignScript (screen.c) builds each top-level packet's name the same
 * way this does -- memcpy("campaign.s0") then strcat(itoa(slot)) -- which
 * only ever fits a single appended digit in its 13-byte buffer, so retail
 * itself never uses a slot past 9. A stock Kilrathi Saga GAMEDAT carries
 * exactly three: CAMPAIGN.S00 (the WC2 base campaign), .S01 and .S02
 * (Special Operations 1 and 2), all in the one data set.
 *
 * Each campaign.sN packet is a top-level "SCSP" container that itself only
 * carries a couple of lines of its own TEXT resource; the actual dialogue
 * lives across ~50-55 further "FILM" sub-packets it references by name (one
 * per story beat), each of which is its own SCSP container with its own
 * TEXT chunk. GetPackedStringByIndex does the (non-trivial, packed string
 * table) work of resolving each film's real on-disk filename/section; this
 * loads each with FetchDiskPacketRetrying (the same non-fatal, retrying
 * loader already used for the top-level packet) rather than
 * LoadCachedCutsceneResource, whose own failure path is fatal to the whole
 * process once "high memory" mode has latched on -- real gameplay never
 * hits that because the cutscene script only ever requests indices it
 * knows are actually present, but brute-forcing every index in the table
 * can walk past one that doesn't actually resolve. */

char *g_pszSdlSubtitleDumpDir;

static char g_szSdlSubtitleDumpCampaignDir[512];
static int g_nSdlSubtitleDumpCounter;
static FILE *g_pSdlSubtitleDumpLog;

static void SdlSubtitleDumpLog(const char *format, ...)
{
    va_list args;

    if (g_pSdlSubtitleDumpLog == 0)
        return;
    va_start(args, format);
    vfprintf(g_pSdlSubtitleDumpLog, format, args);
    va_end(args);
    fflush(g_pSdlSubtitleDumpLog);
}

void SdlDumpCutsceneTextEntries(char **entries, short count)
{
    short index;
    char path[600];
    FILE *file;

    if (g_pszSdlSubtitleDumpDir == 0)
        return;
    for (index = 0; index < count; index++) {
        if (entries[index] == 0)
            continue;
        sprintf(path, "%s/%02d.txt", g_szSdlSubtitleDumpCampaignDir,
                g_nSdlSubtitleDumpCounter);
        file = fopen(path, "w");
        if (file != 0) {
            fputs(entries[index], file);
            fclose(file);
        } else {
            SdlSubtitleDumpLog("Could not write %s\n", path);
        }
        g_nSdlSubtitleDumpCounter++;
    }
}

static void SdlDumpCampaignFilmScenes(void)
{
    CutsceneResourceTable *films;
    short index;
    void *filmPacket;
    char *filmName;

    films = g_pCutsceneFilmResources_004928b4;
    if (films == 0)
        return;
    for (index = 0; index < films->count; index++) {
        filmName = GetPackedStringByIndex(films, films->filenameIndices[index]);
        SdlSubtitleDumpLog("  film %d/%d: %s section %d -- before load\n",
                           index, films->count, filmName,
                           films->sectionIndices[index]);
        /* LoadCachedCutsceneResource's own failure path
         * (ReportCutscenePacketAllocationFailure / FatalCutsceneError) is
         * fatal to the whole process once "high memory" mode has latched
         * on, which real gameplay never hits because the cutscene script
         * only ever requests indices it knows are actually present.
         * Brute-forcing every index in the table can walk past one that
         * doesn't actually resolve (GetNamedPacketSize agreeing the name
         * exists is not the same as this exact section loading cleanly).
         * FetchDiskPacketRetrying is the same non-fatal, retrying loader
         * already used for the top-level campaign.sN packet -- reuse it
         * here too instead of the caching-and-fatal-on-miss loader. */
        filmPacket = FetchDiskPacketRetrying(
            filmName, (short)films->sectionIndices[index], 0);
        SdlSubtitleDumpLog("  film %d/%d: %s -- loaded, packet=%p\n", index,
                           films->count, filmName, filmPacket);
        if (filmPacket != 0) {
            /* Mirrors screens.c's own nested-FILM opcode (0x89): without
             * bumping the resource level, every nested parse pushes onto
             * the SAME level as the top-level packet and its predecessors,
             * so SPRT/PLNE/SEQU/SCNE resources from every earlier film
             * accumulate forever instead of being released once that
             * film's finished with. */
            g_nCutsceneResourceLevel_00499d98++;
            g_nActiveCutsceneResourceLevel_00499d9c =
                g_nCutsceneResourceLevel_00499d98;
            ParseCutsceneContainer(filmPacket);
            SdlSubtitleDumpLog("  film %d/%d: %s -- parsed\n", index,
                               films->count, filmName);
            ReleaseCutsceneResourceLevel(g_nCutsceneResourceLevel_00499d98);
            g_nCutsceneResourceLevel_00499d98--;
            g_nActiveCutsceneResourceLevel_00499d9c =
                g_nCutsceneResourceLevel_00499d98;
            ReleasePacketHandle(filmPacket);
        }
    }
}

void SdlRunSubtitleDumpAndExit(void)
{
    short slot;
    char packetName[13];
    char numberText[4];
    void *scenePacket;
    short campaignsFound;

    g_pSdlSubtitleDumpLog = fopen("subtitle_dump_log.txt", "w");

    /* AllocateViewport (music.c) fatally exits via LogDisplayMode unless
     * g_nSpacePaletteFadeMode_004901e8 == 0x13; normally set on entering
     * space-flight/cutscene mode, which this driver bypasses entirely by
     * jumping straight into cutscene parsing. */
    ConfigureDefaultSpacePalette(0);
    /* A stock Kilrathi Saga GAMEDAT only ever carries 3 real campaigns
     * (0=WC2 base, 1=Special Operations 1, 2=Special Operations 2) -- a
     * higher slot's packet index entry can exist without resolving to
     * real, loadable content, which crashes deep inside cutscene resource
     * loading with no further diagnostic output. Capped here rather than
     * chased further since it is out of scope for what was asked for. */
    campaignsFound = 0;
    for (slot = 0; slot < 3; slot++) {
        memcpy(packetName, "campaign.s0", 12);
        strcat(packetName, _itoa((int)slot, numberText, 10));
        if (GetNamedPacketSize(packetName, 0) == 0)
            continue;
        sprintf(g_szSdlSubtitleDumpCampaignDir, "%s/campaign-%02d",
                g_pszSdlSubtitleDumpDir, slot);
        _mkdir(g_szSdlSubtitleDumpCampaignDir);
        g_nSdlSubtitleDumpCounter = 0;
        SdlSubtitleDumpLog("=== slot %d (%s) starting ===\n", slot,
                           packetName);
        scenePacket = FetchDiskPacketRetrying(packetName, 0, 0);
        if (scenePacket != 0) {
            ParseCutsceneContainer(scenePacket);
            SdlSubtitleDumpLog("slot %d: top-level parsed, %d entries so far\n",
                               slot, g_nSdlSubtitleDumpCounter);
            SdlDumpCampaignFilmScenes();
            ReleasePacketHandle(scenePacket);
            /* The top-level campaign.sN parse itself runs at resource
             * level 0 (RunCampaignScript never bumps the level around it
             * either -- only nested sub-scene loads do), and nothing else
             * in a single normal playthrough ever calls ParseCutsceneContainer
             * again at level 0 for a *different* campaign in the same
             * process, so retail never needs to release it back out.
             * Looping across all 3 campaigns here does, or level 0's own
             * SPRT/PLNE/SEQU/SCNE/FILM/etc. resources from every earlier
             * campaign keep piling up underneath the next one. */
            ReleaseCutsceneResourceLevel(0);
        }
        SdlSubtitleDumpLog("%s: %d subtitle entries -> %s\n", packetName,
                           g_nSdlSubtitleDumpCounter,
                           g_szSdlSubtitleDumpCampaignDir);
        campaignsFound++;
    }
    SdlSubtitleDumpLog("Dumped %d campaign(s) to %s\n", campaignsFound,
                       g_pszSdlSubtitleDumpDir);
    if (g_pSdlSubtitleDumpLog != 0)
        fclose(g_pSdlSubtitleDumpLog);
    exit(0);
}
