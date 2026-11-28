SkyrimSongs 0.5.11
=================

Play the lute and flute for real, anywhere in Skyrim, with your keyboard or a USB MIDI keyboard. Whoever is in
earshot listens: play well and they toss you coins, play badly and they heckle - and in an inn, a bad enough set
gets you thrown out.

Requirements: SKSE64 and Address Library for SKSE Plugins, both for your game version. One DLL works on:
  - Skyrim SE 1.5.97
  - Skyrim AE 1.6.x (Steam), up to 1.6.1170
  - Skyrim 1.7.99 and 1.7.104 (Steam, 2026): SKSE64 2.3.x and Address Library "All in One" v13 or newer
  - GOG 1.6.1179: the GOG builds of SKSE64 and Address Library
Not Skyrim VR, and not Legendary Edition (Oldrim). No plugin (.esp) needed.

How to play
  - Carry a lute or a flute (the game's own, the Bards College quest ones too; RequireInstrument = 0 in the INI
    lets you play without one). Press \ to take it out (change the key in game: SKSE Menu Framework >
    Skyrim Songs > Change...) and \ or Esc to put it away. Your weapon must be sheathed.
    While you play, every other key (other mods' hotkeys too) is kept from the game; \ or Esc gets you out.
  - 37 notes: three rows of keys are three octaves of the white notes, lowest at the bottom, and I is the top C:
        Q W E R T Y U I    high       (I = top C)
        A S D F G H J      middle
        Z X C V B N M      low        (C D E F G A B)
    Hold Shift for the sharp (the black key to the right). The screen shows all 37 as a piano: each white key
    with its letter, each black key with Shift + letter (S+Z = C#). Up / Down arrows move everything an octave.
  - A MIDI keyboard plugged in before you take out your instrument just works (velocity and the sustain pedal too).
  - Tab opens the songbook (Esc closes it): switch instrument, or pick a song and press Play: it plays itself on
    your instrument, perfectly, with no input (the cheat). Esc stops a song; Esc again puts the instrument away.
  - Your MIDI files: put .mid files in Data\SKSE\Plugins\SkyrimSongs\Songs.

The crowd judges free play on rhythm (a steady beat), key (staying in one scale), melody (steps and small leaps,
not random jumps), pace and variety; a song from the songbook always goes down well. Tips come faster and bigger the
happier the room and the better your Speech; you gain Speech experience for what you earn.

Translations: Data\Interface\Translations\SkyrimSongs_ENGLISH.txt has every line of text. Copy it to
SkyrimSongs_<LANGUAGE>.txt (RUSSIAN, GERMAN, CHINESE... the game's language), translate the text after each tab,
keep the keys and {} placeholders, save as UTF-16 LE or UTF-8. Missing lines stay English. Details in the file.

Settings: in game with SKSE Menu Framework (Mod Control Panel > Skyrim Songs), or Data\SKSE\Plugins\SkyrimSongs.ini.
Log: Documents\My Games\Skyrim Special Edition\SKSE\SkyrimSongs.log

Source and license (GPL-3.0): https://github.com/evodube/skyrimsongs
Fonts (OFL): Barlow Condensed, IM Fell English.
