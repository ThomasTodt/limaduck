#!/usr/bin/env python3
import sys
import re
import os

# Set of primary key / sequential ID patterns
PK_PATTERNS = re.compile(r'^(id|.*_id)$')

# Small lookup table pairs (ID and name mapping columns)
LOOKUP_PAIRS = {
    ('rt_id', 'rt_role'),
    ('it_id', 'it_info'),
    ('kt_id', 'kt_kind'),
    ('ct_id', 'ct_kind'),
    ('lt_id', 'lt_link'),
    ('cct_sub_id', 'cc_subject_kind'),
    ('cct_stat_id', 'cc_status_kind')
}

def parse_dc_line(line):
    # Regex to extract constraints like: !(col1 OP col1 & col2 OP col2 & )
    match = re.search(r'!\(\s*(.*?)\s*\)', line)
    if not match:
        return None
    
    raw_preds = match.group(1).split('&')
    preds = []
    for p in raw_preds:
        p = p.strip()
        if not p:
            continue
        # Split by operator: ==, !=, >=, <=, >, <
        op_match = re.search(r'([a-zA-Z0-9_]+)\s*(==|!=|>=|<=|>|<)\s*([a-zA-Z0-9_]+)', p)
        if op_match:
            col_left = op_match.group(1)
            op = op_match.group(2)
            col_right = op_match.group(3)
            preds.append((col_left, op, col_right))
    return preds

def classify_dc(preds):
    if not preds:
        return "Empty/Invalid", True
    
    # Extract unique attributes involved in this DC
    attributes = set()
    operators = set()
    for col_left, op, col_right in preds:
        attributes.add(col_left)
        attributes.add(col_right)
        operators.add(op)
    
    # Case 1: Trivial single-column constraint (contains no relationship between columns)
    if len(attributes) <= 1:
        return "Single-column Triviality", True
    
    # Case 2: Trivial lookup name mappings (e.g. rt_id -> rt_role)
    if len(attributes) == 2:
        attr_list = sorted(list(attributes))
        if (attr_list[0], attr_list[1]) in LOOKUP_PAIRS or (attr_list[1], attr_list[0]) in LOOKUP_PAIRS:
            return "Lookup Name Mapping", True

    # Case 3: Spurious Order Dependency on sequential IDs
    # If the DC only consists of ordering comparisons (<, <=, >, >=) between a PK/ID column and another column
    if all(op in {'<', '<=', '>', '>='} for op in operators):
        has_pk = any(PK_PATTERNS.match(attr) for attr in attributes)
        if has_pk:
            return "Spurious Order Dependency (Sequential sorting artifact)", True

    return "Potentially Useful", False

def process_file(filepath):
    if not os.path.exists(filepath):
        print(f"Error: File not found at {filepath}")
        return

    print(f"Parsing LIMA output file: {filepath}\n")
    
    current_section = "Unknown"
    results = {}
    
    with open(filepath, 'r') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            
            # Identify sections like: --- join_cast_info_name ---
            if line.startswith("---") and line.endswith("---"):
                current_section = line.strip("- ")
                results[current_section] = {
                    "total": 0,
                    "useless": [],
                    "useful": []
                }
                continue
            
            # Check if line is a DC
            if line.startswith("!(") and line.endswith(")"):
                if current_section not in results:
                    results[current_section] = {"total": 0, "useless": [], "useful": []}
                
                preds = parse_dc_line(line)
                category, is_useless = classify_dc(preds)
                
                results[current_section]["total"] += 1
                if is_useless:
                    results[current_section]["useless"].append((line, category))
                else:
                    results[current_section]["useful"].append(line)

    # Print Summary Report
    grand_total_useful = 0
    grand_total_useless = 0
    
    for section, stats in results.items():
        if stats["total"] == 0:
            continue
        
        useful_cnt = len(stats["useful"])
        useless_cnt = len(stats["useless"])
        grand_total_useful += useful_cnt
        grand_total_useless += useless_cnt
        
        print("=" * 60)
        print(f" SECTION: {section}")
        print(f" Total DCs Discovered: {stats['total']}")
        print(f" Useful DCs: {useful_cnt} | Useless DCs: {useless_cnt}")
        print("=" * 60)
        
        if useless_cnt > 0:
            print("\n--- USELESS DCs DETECTED ---")
            # Group by category for cleaner display
            by_category = {}
            for dc, cat in stats["useless"]:
                by_category.setdefault(cat, []).append(dc)
            for cat, dcs in by_category.items():
                print(f" * Category: {cat} ({len(dcs)} found)")
                for dc in dcs[:5]:  # show up to 5 examples
                    print(f"   - {dc}")
                if len(dcs) > 5:
                    print(f"   - ... and {len(dcs)-5} more")
        
        if useful_cnt > 0:
            print("\n--- USEFUL DCs ---")
            for dc in stats["useful"]:
                print(f" * {dc}")
        else:
            print("\nNo non-trivial DCs found in this section.")
        print()

    print("=" * 60)
    print(" SUMMARY")
    print("=" * 60)
    print(f" Grand Total Useful DCs: {grand_total_useful}")
    print(f" Grand Total Useless DCs: {grand_total_useless}")
    print("=" * 60)

if __name__ == "__main__":
    target_file = "testes/resultados/test_job_joins_01.txt"
    if len(sys.argv) > 1:
        target_file = sys.argv[1]
    process_file(target_file)
