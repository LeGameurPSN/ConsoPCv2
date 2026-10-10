# ⚡ PowerMon GUI v2.0

**Moniteur de consommation électrique — Windows 11 — GUI natif Win32 + Direct2D**

> Application fenêtrée, zéro CMD, zéro dépendance externe.  
> Lance `powermon.exe` et c'est tout.

---

## Captures d'écran (aperçu du rendu)

```
┌──────────────────────────────────────────────────────────────────┐
│ ⚡ POWERMON          Session 00:04:32                    14:22:07 │
├──────────────────────────────────────────────────────────────────┤
│ PUISSANCE TOTALE ESTIMÉE                                         │
│  187.4 W              CPU ██████░░░░░░  41.7W                    │
│  Min 142  Moy 164  Max 212  GPU ████████░░░░ 144.0W             │
│                              RAM █████░░░░░░  7.0W              │
├──────────────────────────────────────────────────────────────────┤
│ [graphe 120s en temps réel ─────────────────────────────▲──────] │
├──────────────────────┬───────────────────────────────────────────┤
│ AMD Ryzen 7 5800X    │ AMD Radeon RX 580 Series                 │
│ 41.7 W               │ 144.0 W                                  │
│ 42% ████░░ 4200 MHz  │ 78% ████████░░ | 8.0 Go VRAM            │
├──────────────────────┼───────────────────────────────────────────┤
│ MÉMOIRE RAM          │ STOCKAGE NVMe/SSD                        │
│ 7.0 W                │ 1.2 W                                    │
│ 11.2/16.0 Go (70%)   │ Lect 45 Mo/s | Écrit 12 Mo/s            │
├──────────────────────┼───────────────────────────────────────────┤
│ RÉSEAU               │ AFFICHAGE                                │
│ 2.5 W                │ 50 W                                     │
│ ↑ 120 Ko/s ↓ 890     │ 2 écrans détectés                       │
├──────────────────────────────────────────────────────────────────┤
│ USB (4 périphériques — ~5.5W)                                    │
│ USB Audio Device    ~1.0W   HID Mouse          ~0.5W            │
│ HID Keyboard        ~0.5W   USB Storage        ~4.5W            │
├──────────────────────────────────────────────────────────────────┤
│ BILAN  Cumulé 12.3456 Wh  Proj. 24h: 0.988 €   Mois: 29.64 €  │
│                                      COÛT SESSION  0.00311 €    │
└──────────────────────────────────────────────────────────────────┘
```

---

## Télécharger

> **[⬇ powermon.exe — Dernière release](../../releases/latest)**

Aucune installation. Aucun vc_redist. Aucune dépendance.  
Double-clic → la fenêtre s'ouvre.

---

## Capteurs utilisés

| Composant | Source Windows | Données lues |
|-----------|---------------|--------------|
| **CPU %** | PDH `\Processor(_Total)\% Processor Time` | Usage réel noyau |
| **CPU fréquence** | WMI `Win32_Processor.CurrentClockSpeed` | MHz courant |
| **CPU nom / cœurs** | WMI `Win32_Processor.Name` | Modèle + nb cœurs |
| **GPU %** | PDH `\GPU Engine(*)\Utilization Percentage` | Somme engines |
| **GPU nom + VRAM** | DXGI `IDXGIAdapter::GetDesc` | Nom + VRAM dédiée |
| **RAM usage** | `GlobalMemoryStatusEx` | Go utilisés / total |
| **Disque** | PDH `\PhysicalDisk(_Total)\Disk Read/Write Bytes/sec` | Mo/s lecture/écriture |
| **Réseau** | PDH `\Network Interface(*)\Bytes Sent/Received/sec` | Ko/s toutes interfaces |
| **Batterie** | `GetSystemPowerStatus` | %, charge, secteur |
| **USB** | `SetupDiGetClassDevs` (GUID_DEVCLASS_USB) | Liste réelle branchée |
| **Écrans** | `GetSystemMetrics(SM_CMONITORS)` | Nombre d'écrans |

**Puissances** = TDP constructeur × % usage (modèle linéaire, ±15W typique)

---

## Réglages (sans recompiler)

Clic droit dans la fenêtre → **⚙ Réglages TDP / Tarif…**  
ou touche **F5**

| Paramètre | Valeur par défaut | Description |
|-----------|------------------|-------------|
| TDP CPU max | 95 W | TDP 100% selon ta fiche technique |
| TDP GPU max | 185 W | RX 580 TDP officiel AMD |
| Système base | 45 W | Carte mère + stockage + ventilos |
| Tarif €/kWh | 0.2516 | EDF HP 2024 — modifie selon ton contrat |

**Les changements sont immédiats, sans redémarrer l'app.**

---

## Compiler

### Prérequis
- Visual Studio 2022 (composant "Développement Desktop C++")
- CMake ≥ 3.20

```bat
:: Developer Command Prompt x64
cmake -B build -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release
cmake --build build
build\powermon.exe
```

Le flag `/MT` est activé : runtime C++ lié statiquement → exe autonome.

---

## GitHub Actions

Push sur `main` → compile automatiquement → artifact `powermon-windows-x64-gui` disponible dans l'onglet **Actions**.

Pour une release publique avec l'exe en pièce jointe :
```
git tag v2.0.0 && git push --tags
# Créer la Release GitHub → powermon.exe s'attache automatiquement
```

---

## Roadmap

- [ ] ADL SDK → puissance GPU AMD réelle (registre matériel)
- [ ] Lecture températures via MSI Afterburner shared memory
- [ ] Shelly Plug S — puissance prise réelle via API REST
- [ ] Export CSV/JSON continu
- [ ] Thème clair / foncé
- [ ] Tray icon (mini-fenêtre en fond)
