import os
import json
import xml.etree.ElementTree as ET
import glob

SOURCE_DIR = "../UserXml"
DEST_DIR = "../UserJson"

def ensure_dir(d):
    if not os.path.exists(d):
        os.makedirs(d)

def main():
    if not os.path.exists(SOURCE_DIR):
        print(f"Source directory {SOURCE_DIR} not found.")
        return

    users = []
    xml_files = glob.glob(os.path.join(SOURCE_DIR, "*.xml"))
    
    print(f"Found {len(xml_files)} XML files.")

    for f in xml_files:
        try:
            tree = ET.parse(f)
            root = tree.getroot()
            
            user_data = {}
            for child in root:
                # Convert tag to lowercase key (or match C++ expectation)
                # C++ CspUser::Parse looks for "Id", "PassWord", "DND", "CallForward", "GroupId"
                # JSON keys should match what we plan to parse in C++
                # Let's use lowercase keys for JSON standard, and update C++ to match: "id", "password"
                # Wait, user request said: { "id" : "1000", "passwd" :"1234", "email":"", ...}
                # User asked for "passwd" logic. I should follow that or "password"? 
                # Request example: { "id" : "1000", "passwd" :"1234", ...}
                # Current XML: <PassWord>
                
                tag = child.tag
                val = child.text if child.text else ""
                
                if tag == "Id":
                    user_data["id"] = val
                elif tag == "PassWord":
                    user_data["passwd"] = val
                elif tag == "DND":
                    user_data["icb_all"] = val  # 착신 차단 — 전체 (TS 24.611 ICB), "true"/"false"
                elif tag == "CallForward":
                    user_data["call_forward"] = val
                elif tag == "GroupId":
                    user_data["group_id"] = val
                else:
                    user_data[tag.lower()] = val
            
            if "id" in user_data:
                users.append(user_data)
        except Exception as e:
            print(f"Error parsing {f}: {e}")

    # Sort by ID
    users.sort(key=lambda u: int(u["id"]))

    # Group by 100
    grouped = {}
    for user in users:
        uid = int(user["id"])
        block_start = (uid // 100) * 100
        if block_start not in grouped:
            grouped[block_start] = []
        grouped[block_start].append(user)

    print(f"Grouped into {len(grouped)} blocks.")

    # Write JSON files
    for block_start, block_users in grouped.items():
        # Path generation
        # ID: 1000 -> 0000001000
        # Directory: 00/00/00/10
        # File: 0000001000.json
        
        padded_id = f"{block_start:010d}"
        
        # Split into chunks of 2 for directory
        # 00 00 00 10 00 -> 00/00/00/10
        
        d1 = padded_id[0:2]
        d2 = padded_id[2:4]
        d3 = padded_id[4:6]
        d4 = padded_id[6:8]
        
        dir_path = os.path.join(DEST_DIR, d1, d2, d3, d4)
        ensure_dir(dir_path)
        
        filename = f"{padded_id}.json"
        file_path = os.path.join(dir_path, filename)
        
        with open(file_path, "w") as f:
            json.dump(block_users, f, indent=4)
        
        print(f"Written {file_path} with {len(block_users)} users.")

if __name__ == "__main__":
    main()
