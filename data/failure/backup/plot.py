import pandas as pd
import matplotlib.pyplot as plt

def plot_throughput(file1, file2):
    """
    Reads two CSV files containing throughput data and plots a graph with two lines.

    Args:
        file1 (str): Path to the first CSV file (e.g., "real_time_throughput.csv").
        file2 (str): Path to the second CSV file (e.g., "no_failure.csv").
    """
    try:
        # Read the CSV files into pandas DataFrames
        df1 = pd.read_csv(file1)
        df2 = pd.read_csv(file2)

        # Check if the required column exists
        if "RealTimeThroughput" not in df1.columns or "RealTimeThroughput" not in df2.columns:
            raise ValueError("The CSV files must contain a column named 'RealTimeThroughput'.")

        # Create the plot with publication-quality aesthetics
        plt.style.use('seaborn-v0_8-whitegrid')  # Use a nice style
        plt.figure(figsize=(12, 7), dpi=300)  # Set the figure size and resolution

        # Plot the data from the first file
        plt.plot(df1.index * 10, df1["RealTimeThroughput"],
                 label="Failure Throughput", color='blue', marker='o',
                 linestyle='-', markersize=5, linewidth=2)

        # Plot the data from the second file
        plt.plot(df2.index * 10, df2["RealTimeThroughput"],
                 label="No Failure Throughput", color='orange', marker='x',
                 linestyle='--', markersize=5, linewidth=2)

        # Add labels and title with increased font sizes
        plt.xlabel("Time (milliseconds)", fontsize=14)
        plt.ylabel("Throughput (MB/s)", fontsize=14)  # Specify units if needed
        plt.title("Real-time Throughput Broker failure vs no failure", fontsize=16)

        # Customize ticks and grid
        plt.xticks(fontsize=12)
        plt.yticks(fontsize=12)
        plt.grid(True, which='both', linestyle='--', linewidth=0.7)

        # Add legend with improved placement and font size
        plt.legend(fontsize=12, loc='upper right', frameon=True)

        # Save the figure with a clear filename
        plt.savefig('failure.pdf', dpi=300, bbox_inches='tight')

        # Show the plot (optional, can be commented out in publication context)
        # plt.show()

    except FileNotFoundError:
        print(f"Error: One or both of the files were not found. Please check the file paths: {file1}, {file2}")
    except pd.errors.EmptyDataError:
        print(f"Error: One or both CSV files are empty.")
    except ValueError as ve:
        print(f"ValueError: {ve}")
    except Exception as e:
        print(f"An unexpected error occurred: {e}")

# Example usage
if __name__ == "__main__":
    file1_path = "real_time_throughput.csv"
    file2_path = "no_failure.csv"
    plot_throughput(file1_path, file2_path)
