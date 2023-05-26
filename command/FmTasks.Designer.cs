namespace command
{
    partial class FmTasks
    {
        /// <summary>
        /// Required designer variable.
        /// </summary>
        private System.ComponentModel.IContainer components = null;

        /// <summary>
        /// Clean up any resources being used.
        /// </summary>
        /// <param name="disposing">true if managed resources should be disposed; otherwise, false.</param>
        protected override void Dispose(bool disposing)
        {
            if (disposing && (components != null))
            {
                components.Dispose();
            }
            base.Dispose(disposing);
        }

        #region Windows Form Designer generated code

        /// <summary>
        /// Required method for Designer support - do not modify
        /// the contents of this method with the code editor.
        /// </summary>
        private void InitializeComponent()
        {
            System.ComponentModel.ComponentResourceManager resources = new System.ComponentModel.ComponentResourceManager(typeof(FmTasks));
            this.txt_TaskName = new System.Windows.Forms.TextBox();
            this.label1 = new System.Windows.Forms.Label();
            this.txt_TaskValue = new System.Windows.Forms.TextBox();
            this.label2 = new System.Windows.Forms.Label();
            this.bt_Save = new System.Windows.Forms.Button();
            this.bt_Del = new System.Windows.Forms.Button();
            this.lv_Data = new System.Windows.Forms.ListView();
            this.columnHeader1 = ((System.Windows.Forms.ColumnHeader)(new System.Windows.Forms.ColumnHeader()));
            this.columnHeader3 = ((System.Windows.Forms.ColumnHeader)(new System.Windows.Forms.ColumnHeader()));
            this.columnHeader2 = ((System.Windows.Forms.ColumnHeader)(new System.Windows.Forms.ColumnHeader()));
            this.bt_New = new System.Windows.Forms.Button();
            this.SuspendLayout();
            // 
            // txt_TaskName
            // 
            this.txt_TaskName.Location = new System.Drawing.Point(72, 12);
            this.txt_TaskName.Name = "txt_TaskName";
            this.txt_TaskName.Size = new System.Drawing.Size(139, 26);
            this.txt_TaskName.TabIndex = 4;
            // 
            // label1
            // 
            this.label1.AutoSize = true;
            this.label1.Location = new System.Drawing.Point(12, 17);
            this.label1.Name = "label1";
            this.label1.Size = new System.Drawing.Size(56, 16);
            this.label1.TabIndex = 3;
            this.label1.Text = "任务名";
            // 
            // txt_TaskValue
            // 
            this.txt_TaskValue.Location = new System.Drawing.Point(72, 44);
            this.txt_TaskValue.Name = "txt_TaskValue";
            this.txt_TaskValue.Size = new System.Drawing.Size(139, 26);
            this.txt_TaskValue.TabIndex = 6;
            // 
            // label2
            // 
            this.label2.AutoSize = true;
            this.label2.Location = new System.Drawing.Point(42, 49);
            this.label2.Name = "label2";
            this.label2.Size = new System.Drawing.Size(24, 16);
            this.label2.TabIndex = 5;
            this.label2.Text = "值";
            // 
            // bt_Save
            // 
            this.bt_Save.Location = new System.Drawing.Point(242, 10);
            this.bt_Save.Name = "bt_Save";
            this.bt_Save.Size = new System.Drawing.Size(75, 31);
            this.bt_Save.TabIndex = 7;
            this.bt_Save.Text = "保存";
            this.bt_Save.UseVisualStyleBackColor = true;
            this.bt_Save.Click += new System.EventHandler(this.bt_Save_Click);
            // 
            // bt_Del
            // 
            this.bt_Del.Enabled = false;
            this.bt_Del.Location = new System.Drawing.Point(242, 45);
            this.bt_Del.Name = "bt_Del";
            this.bt_Del.Size = new System.Drawing.Size(75, 31);
            this.bt_Del.TabIndex = 8;
            this.bt_Del.Text = "删除";
            this.bt_Del.UseVisualStyleBackColor = true;
            this.bt_Del.Click += new System.EventHandler(this.bt_Del_Click);
            // 
            // lv_Data
            // 
            this.lv_Data.Anchor = ((System.Windows.Forms.AnchorStyles)((((System.Windows.Forms.AnchorStyles.Top | System.Windows.Forms.AnchorStyles.Bottom) 
            | System.Windows.Forms.AnchorStyles.Left) 
            | System.Windows.Forms.AnchorStyles.Right)));
            this.lv_Data.Columns.AddRange(new System.Windows.Forms.ColumnHeader[] {
            this.columnHeader1,
            this.columnHeader3,
            this.columnHeader2});
            this.lv_Data.Font = new System.Drawing.Font("宋体", 12F);
            this.lv_Data.FullRowSelect = true;
            this.lv_Data.GridLines = true;
            this.lv_Data.HideSelection = false;
            this.lv_Data.Location = new System.Drawing.Point(1, 82);
            this.lv_Data.MultiSelect = false;
            this.lv_Data.Name = "lv_Data";
            this.lv_Data.Size = new System.Drawing.Size(442, 483);
            this.lv_Data.TabIndex = 144;
            this.lv_Data.UseCompatibleStateImageBehavior = false;
            this.lv_Data.View = System.Windows.Forms.View.Details;
            this.lv_Data.SelectedIndexChanged += new System.EventHandler(this.lv_Data_SelectedIndexChanged);
            // 
            // columnHeader1
            // 
            this.columnHeader1.Text = "id";
            this.columnHeader1.Width = 126;
            // 
            // columnHeader3
            // 
            this.columnHeader3.Text = "任务名";
            this.columnHeader3.Width = 180;
            // 
            // columnHeader2
            // 
            this.columnHeader2.Text = "值";
            this.columnHeader2.Width = 123;
            // 
            // bt_New
            // 
            this.bt_New.Location = new System.Drawing.Point(336, 11);
            this.bt_New.Name = "bt_New";
            this.bt_New.Size = new System.Drawing.Size(75, 31);
            this.bt_New.TabIndex = 145;
            this.bt_New.Text = "新建";
            this.bt_New.UseVisualStyleBackColor = true;
            this.bt_New.Click += new System.EventHandler(this.bt_New_Click);
            // 
            // FmTasks
            // 
            this.AutoScaleDimensions = new System.Drawing.SizeF(8F, 16F);
            this.AutoScaleMode = System.Windows.Forms.AutoScaleMode.Font;
            this.ClientSize = new System.Drawing.Size(446, 565);
            this.Controls.Add(this.bt_New);
            this.Controls.Add(this.lv_Data);
            this.Controls.Add(this.bt_Del);
            this.Controls.Add(this.bt_Save);
            this.Controls.Add(this.txt_TaskValue);
            this.Controls.Add(this.label2);
            this.Controls.Add(this.txt_TaskName);
            this.Controls.Add(this.label1);
            this.Font = new System.Drawing.Font("宋体", 12F);
            this.Icon = ((System.Drawing.Icon)(resources.GetObject("$this.Icon")));
            this.Margin = new System.Windows.Forms.Padding(4, 4, 4, 4);
            this.MaximizeBox = false;
            this.Name = "FmTasks";
            this.StartPosition = System.Windows.Forms.FormStartPosition.CenterParent;
            this.Text = "任务名、值";
            this.ResumeLayout(false);
            this.PerformLayout();

        }

        #endregion

        private System.Windows.Forms.TextBox txt_TaskName;
        private System.Windows.Forms.Label label1;
        private System.Windows.Forms.TextBox txt_TaskValue;
        private System.Windows.Forms.Label label2;
        private System.Windows.Forms.Button bt_Save;
        private System.Windows.Forms.Button bt_Del;
        private System.Windows.Forms.ListView lv_Data;
        private System.Windows.Forms.ColumnHeader columnHeader1;
        private System.Windows.Forms.ColumnHeader columnHeader3;
        private System.Windows.Forms.ColumnHeader columnHeader2;
        private System.Windows.Forms.Button bt_New;
    }
}