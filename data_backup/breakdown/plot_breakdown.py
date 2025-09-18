import os
import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
import re

# Set Seaborn and matplotlib for publication-quality plots
sns.set(style="whitegrid")
plt.rcParams.update({
    "text.usetex": False,
    "font.family": "serif",
    "font.size": 14,
    "figure.dpi": 300
})

# === Configuration ===
# CSV files in current directory
csv_files = [
    "CORFU_2_0_latency.csv",
    "CORFU_2_1_latency.csv",
    "EMBARCADERO_0_0_latency.csv",
    "EMBARCADERO_4_0_latency.csv",
    "EMBARCADERO_4_1_latency.csv",
    "SCALOG_1_0_latency.csv",
    "SCALOG_1_1_latency.csv"
]

# Group colors
group_colors = {
    "CORFU": sns.color_palette("Blues", 3),
    "EMBARCADERO": sns.color_palette("Greens", 3),
    "SCALOG": sns.color_palette("Reds", 3),
}

# === Helper function ===
def parse_filename(filename):
    match = re.match(r"([A-Z]+)_(\d+)_(\d+)_latency\.csv", filename)
    if match:
        system, config1, config2 = match.groups()
        return system, f"{config1}_{config2}"
    else:
        raise ValueError(f"Filename {filename} does not match expected pattern.")

# === Read data ===
data = []
for file in csv_files:
    system, config = parse_filename(file)
    df = pd.read_csv(file)
    avg = df["Average"].iloc[0]
    data.append({
        "System": system,
        "Config": config,
        "Label": f"{system}_{config}",
        "Average": avg
    })

df_all = pd.DataFrame(data)

# === Sort and prepare for plotting ===
df_all["System"] = pd.Categorical(df_all["System"], categories=["CORFU", "EMBARCADERO", "SCALOG"], ordered=True)
df_all = df_all.sort_values(by=["System", "Config"])

# Assign colors
color_map = []
color_idx = {"CORFU": 0, "EMBARCADERO": 0, "SCALOG": 0}
for _, row in df_all.iterrows():
    sys = row["System"]
    color = group_colors[sys][color_idx[sys]]
    color_map.append(color)
    color_idx[sys] += 1

# === Plotting ===
fig, ax = plt.subplots(figsize=(10, 6))

# Calculate spacing between groups
bar_positions = []
labels = []
current_pos = 0
group_spacing = 1.5
bar_width = 0.8

last_system = None
for idx, row in df_all.iterrows():
    if row["System"] != last_system and last_system is not None:
        current_pos += group_spacing
    bar_positions.append(current_pos)
    labels.append(row["Label"])
    current_pos += 1  # next bar
    last_system = row["System"]

# Plot bars
bars = ax.bar(bar_positions, df_all["Average"], color=color_map, width=bar_width)

# Labeling
#ax.set_ylabel("Average Latency (ms)", fontsize=14)
ax.set_xticks(bar_positions)
ax.set_xticklabels(labels, rotation=45, ha="right", fontsize=12)
ax.set_title("Average Latency per Configuration", fontsize=16)

# Remove top and right borders
sns.despine()

# Tight layout
plt.tight_layout()

# Save to file
plt.savefig("breakdown.pdf", format="pdf", dpi=300)

# Show plot (optional)
plt.show()
