namespace command
{
    partial class FmMain
    {
        /// <summary>
        /// 必需的设计器变量。
        /// </summary>
        private System.ComponentModel.IContainer components = null;

        /// <summary>
        /// 清理所有正在使用的资源。
        /// </summary>
        /// <param name="disposing">如果应释放托管资源，为 true；否则为 false。</param>
        protected override void Dispose(bool disposing)
        {
            if (disposing && (components != null))
            {
                components.Dispose();
            }
            base.Dispose(disposing);
        }

        #region Windows 窗体设计器生成的代码

        /// <summary>
        /// 设计器支持所需的方法 - 不要修改
        /// 使用代码编辑器修改此方法的内容。
        /// </summary>
        private void InitializeComponent()
        {
            System.ComponentModel.ComponentResourceManager resources = new System.ComponentModel.ComponentResourceManager(typeof(FmMain));
            this.splitContainer1 = new System.Windows.Forms.SplitContainer();
            this.tabControl1 = new System.Windows.Forms.TabControl();
            this.tabPage1 = new System.Windows.Forms.TabPage();
            this.bt_HarborDel = new System.Windows.Forms.Button();
            this.txt_HarborSearch = new System.Windows.Forms.TextBox();
            this.bt_HarborRun = new System.Windows.Forms.Button();
            this.bt_HarborSave = new System.Windows.Forms.Button();
            this.txt_HarborCMD = new System.Windows.Forms.TextBox();
            this.cb_HarborTask = new System.Windows.Forms.ComboBox();
            this.label4 = new System.Windows.Forms.Label();
            this.label3 = new System.Windows.Forms.Label();
            this.bt_HarborLink = new System.Windows.Forms.Button();
            this.txt_HarborPort = new System.Windows.Forms.TextBox();
            this.txt_HarborIp = new System.Windows.Forms.TextBox();
            this.label2 = new System.Windows.Forms.Label();
            this.label1 = new System.Windows.Forms.Label();
            this.tabPage2 = new System.Windows.Forms.TabPage();
            this.txt_Show = new System.Windows.Forms.TextBox();
            this.menuStrip1 = new System.Windows.Forms.MenuStrip();
            this.操作ToolStripMenuItem = new System.Windows.Forms.ToolStripMenuItem();
            this.任务ToolStripMenuItem = new System.Windows.Forms.ToolStripMenuItem();
            this.lv_Name = new System.Windows.Forms.ListView();
            this.columnHeader1 = ((System.Windows.Forms.ColumnHeader)(new System.Windows.Forms.ColumnHeader()));
            this.textBox1 = new System.Windows.Forms.TextBox();
            this.label5 = new System.Windows.Forms.Label();
            this.groupBox1 = new System.Windows.Forms.GroupBox();
            this.groupBox2 = new System.Windows.Forms.GroupBox();
            this.groupBox3 = new System.Windows.Forms.GroupBox();
            this.bt_HarborNew = new System.Windows.Forms.Button();
            ((System.ComponentModel.ISupportInitialize)(this.splitContainer1)).BeginInit();
            this.splitContainer1.Panel1.SuspendLayout();
            this.splitContainer1.Panel2.SuspendLayout();
            this.splitContainer1.SuspendLayout();
            this.tabControl1.SuspendLayout();
            this.tabPage1.SuspendLayout();
            this.menuStrip1.SuspendLayout();
            this.groupBox1.SuspendLayout();
            this.groupBox2.SuspendLayout();
            this.groupBox3.SuspendLayout();
            this.SuspendLayout();
            // 
            // splitContainer1
            // 
            this.splitContainer1.BorderStyle = System.Windows.Forms.BorderStyle.FixedSingle;
            this.splitContainer1.Dock = System.Windows.Forms.DockStyle.Fill;
            this.splitContainer1.Location = new System.Drawing.Point(0, 25);
            this.splitContainer1.Name = "splitContainer1";
            this.splitContainer1.Orientation = System.Windows.Forms.Orientation.Horizontal;
            // 
            // splitContainer1.Panel1
            // 
            this.splitContainer1.Panel1.Controls.Add(this.tabControl1);
            // 
            // splitContainer1.Panel2
            // 
            this.splitContainer1.Panel2.Controls.Add(this.txt_Show);
            this.splitContainer1.Size = new System.Drawing.Size(927, 702);
            this.splitContainer1.SplitterDistance = 481;
            this.splitContainer1.TabIndex = 0;
            // 
            // tabControl1
            // 
            this.tabControl1.Controls.Add(this.tabPage1);
            this.tabControl1.Controls.Add(this.tabPage2);
            this.tabControl1.Dock = System.Windows.Forms.DockStyle.Fill;
            this.tabControl1.Location = new System.Drawing.Point(0, 0);
            this.tabControl1.Name = "tabControl1";
            this.tabControl1.SelectedIndex = 0;
            this.tabControl1.Size = new System.Drawing.Size(925, 479);
            this.tabControl1.TabIndex = 1;
            // 
            // tabPage1
            // 
            this.tabPage1.Controls.Add(this.groupBox3);
            this.tabPage1.Controls.Add(this.groupBox2);
            this.tabPage1.Controls.Add(this.groupBox1);
            this.tabPage1.Controls.Add(this.lv_Name);
            this.tabPage1.Controls.Add(this.txt_HarborCMD);
            this.tabPage1.Location = new System.Drawing.Point(4, 26);
            this.tabPage1.Name = "tabPage1";
            this.tabPage1.Padding = new System.Windows.Forms.Padding(3);
            this.tabPage1.Size = new System.Drawing.Size(917, 449);
            this.tabPage1.TabIndex = 0;
            this.tabPage1.Text = "Harbor";
            this.tabPage1.UseVisualStyleBackColor = true;
            // 
            // bt_HarborDel
            // 
            this.bt_HarborDel.Location = new System.Drawing.Point(348, 16);
            this.bt_HarborDel.Name = "bt_HarborDel";
            this.bt_HarborDel.Size = new System.Drawing.Size(50, 28);
            this.bt_HarborDel.TabIndex = 13;
            this.bt_HarborDel.Text = "删除";
            this.bt_HarborDel.UseVisualStyleBackColor = true;
            this.bt_HarborDel.Click += new System.EventHandler(this.bt_HarborDel_Click);
            // 
            // txt_HarborSearch
            // 
            this.txt_HarborSearch.Location = new System.Drawing.Point(48, 49);
            this.txt_HarborSearch.Name = "txt_HarborSearch";
            this.txt_HarborSearch.Size = new System.Drawing.Size(140, 26);
            this.txt_HarborSearch.TabIndex = 12;
            // 
            // bt_HarborRun
            // 
            this.bt_HarborRun.Enabled = false;
            this.bt_HarborRun.Location = new System.Drawing.Point(196, 27);
            this.bt_HarborRun.Name = "bt_HarborRun";
            this.bt_HarborRun.Size = new System.Drawing.Size(50, 28);
            this.bt_HarborRun.TabIndex = 11;
            this.bt_HarborRun.Text = "执行";
            this.bt_HarborRun.UseVisualStyleBackColor = true;
            this.bt_HarborRun.Click += new System.EventHandler(this.bt_HarborRun_Click);
            // 
            // bt_HarborSave
            // 
            this.bt_HarborSave.Location = new System.Drawing.Point(210, 16);
            this.bt_HarborSave.Name = "bt_HarborSave";
            this.bt_HarborSave.Size = new System.Drawing.Size(50, 28);
            this.bt_HarborSave.TabIndex = 10;
            this.bt_HarborSave.Text = "保存";
            this.bt_HarborSave.UseVisualStyleBackColor = true;
            this.bt_HarborSave.Click += new System.EventHandler(this.bt_HarborSave_Click);
            // 
            // txt_HarborCMD
            // 
            this.txt_HarborCMD.Anchor = ((System.Windows.Forms.AnchorStyles)((((System.Windows.Forms.AnchorStyles.Top | System.Windows.Forms.AnchorStyles.Bottom) 
            | System.Windows.Forms.AnchorStyles.Left) 
            | System.Windows.Forms.AnchorStyles.Right)));
            this.txt_HarborCMD.Location = new System.Drawing.Point(192, 151);
            this.txt_HarborCMD.Multiline = true;
            this.txt_HarborCMD.Name = "txt_HarborCMD";
            this.txt_HarborCMD.ScrollBars = System.Windows.Forms.ScrollBars.Both;
            this.txt_HarborCMD.Size = new System.Drawing.Size(725, 299);
            this.txt_HarborCMD.TabIndex = 9;
            // 
            // cb_HarborTask
            // 
            this.cb_HarborTask.DropDownStyle = System.Windows.Forms.ComboBoxStyle.DropDownList;
            this.cb_HarborTask.FormattingEnabled = true;
            this.cb_HarborTask.Location = new System.Drawing.Point(48, 15);
            this.cb_HarborTask.Name = "cb_HarborTask";
            this.cb_HarborTask.Size = new System.Drawing.Size(140, 24);
            this.cb_HarborTask.TabIndex = 8;
            // 
            // label4
            // 
            this.label4.AutoSize = true;
            this.label4.Location = new System.Drawing.Point(6, 20);
            this.label4.Name = "label4";
            this.label4.Size = new System.Drawing.Size(40, 16);
            this.label4.TabIndex = 7;
            this.label4.Text = "任务";
            // 
            // label3
            // 
            this.label3.AutoSize = true;
            this.label3.Location = new System.Drawing.Point(3, 53);
            this.label3.Name = "label3";
            this.label3.Size = new System.Drawing.Size(40, 16);
            this.label3.TabIndex = 5;
            this.label3.Text = "搜索";
            // 
            // bt_HarborLink
            // 
            this.bt_HarborLink.Location = new System.Drawing.Point(317, 13);
            this.bt_HarborLink.Name = "bt_HarborLink";
            this.bt_HarborLink.Size = new System.Drawing.Size(50, 28);
            this.bt_HarborLink.TabIndex = 4;
            this.bt_HarborLink.Text = "链接";
            this.bt_HarborLink.UseVisualStyleBackColor = true;
            this.bt_HarborLink.Click += new System.EventHandler(this.bt_HarborLink_Click);
            // 
            // txt_HarborPort
            // 
            this.txt_HarborPort.Location = new System.Drawing.Point(222, 14);
            this.txt_HarborPort.Name = "txt_HarborPort";
            this.txt_HarborPort.Size = new System.Drawing.Size(79, 26);
            this.txt_HarborPort.TabIndex = 3;
            // 
            // txt_HarborIp
            // 
            this.txt_HarborIp.Location = new System.Drawing.Point(34, 13);
            this.txt_HarborIp.Name = "txt_HarborIp";
            this.txt_HarborIp.Size = new System.Drawing.Size(142, 26);
            this.txt_HarborIp.TabIndex = 2;
            // 
            // label2
            // 
            this.label2.AutoSize = true;
            this.label2.Location = new System.Drawing.Point(179, 18);
            this.label2.Name = "label2";
            this.label2.Size = new System.Drawing.Size(40, 16);
            this.label2.TabIndex = 1;
            this.label2.Text = "Port";
            // 
            // label1
            // 
            this.label1.AutoSize = true;
            this.label1.Location = new System.Drawing.Point(4, 18);
            this.label1.Name = "label1";
            this.label1.Size = new System.Drawing.Size(24, 16);
            this.label1.TabIndex = 0;
            this.label1.Text = "Ip";
            // 
            // tabPage2
            // 
            this.tabPage2.Location = new System.Drawing.Point(4, 22);
            this.tabPage2.Name = "tabPage2";
            this.tabPage2.Padding = new System.Windows.Forms.Padding(3);
            this.tabPage2.Size = new System.Drawing.Size(917, 386);
            this.tabPage2.TabIndex = 1;
            this.tabPage2.Text = "Echo";
            this.tabPage2.UseVisualStyleBackColor = true;
            // 
            // txt_Show
            // 
            this.txt_Show.Dock = System.Windows.Forms.DockStyle.Fill;
            this.txt_Show.Location = new System.Drawing.Point(0, 0);
            this.txt_Show.Multiline = true;
            this.txt_Show.Name = "txt_Show";
            this.txt_Show.ReadOnly = true;
            this.txt_Show.ScrollBars = System.Windows.Forms.ScrollBars.Both;
            this.txt_Show.Size = new System.Drawing.Size(925, 215);
            this.txt_Show.TabIndex = 0;
            // 
            // menuStrip1
            // 
            this.menuStrip1.Items.AddRange(new System.Windows.Forms.ToolStripItem[] {
            this.操作ToolStripMenuItem});
            this.menuStrip1.Location = new System.Drawing.Point(0, 0);
            this.menuStrip1.Name = "menuStrip1";
            this.menuStrip1.Size = new System.Drawing.Size(927, 25);
            this.menuStrip1.TabIndex = 1;
            this.menuStrip1.Text = "menuStrip1";
            // 
            // 操作ToolStripMenuItem
            // 
            this.操作ToolStripMenuItem.DropDownItems.AddRange(new System.Windows.Forms.ToolStripItem[] {
            this.任务ToolStripMenuItem});
            this.操作ToolStripMenuItem.Name = "操作ToolStripMenuItem";
            this.操作ToolStripMenuItem.Size = new System.Drawing.Size(44, 21);
            this.操作ToolStripMenuItem.Text = "操作";
            // 
            // 任务ToolStripMenuItem
            // 
            this.任务ToolStripMenuItem.Name = "任务ToolStripMenuItem";
            this.任务ToolStripMenuItem.Size = new System.Drawing.Size(100, 22);
            this.任务ToolStripMenuItem.Text = "任务";
            this.任务ToolStripMenuItem.Click += new System.EventHandler(this.任务ToolStripMenuItem_Click);
            // 
            // lv_Name
            // 
            this.lv_Name.Anchor = ((System.Windows.Forms.AnchorStyles)((((System.Windows.Forms.AnchorStyles.Top | System.Windows.Forms.AnchorStyles.Bottom) 
            | System.Windows.Forms.AnchorStyles.Left) 
            | System.Windows.Forms.AnchorStyles.Right)));
            this.lv_Name.Columns.AddRange(new System.Windows.Forms.ColumnHeader[] {
            this.columnHeader1});
            this.lv_Name.Font = new System.Drawing.Font("宋体", 12F);
            this.lv_Name.FullRowSelect = true;
            this.lv_Name.GridLines = true;
            this.lv_Name.HideSelection = false;
            this.lv_Name.Location = new System.Drawing.Point(7, 151);
            this.lv_Name.MultiSelect = false;
            this.lv_Name.Name = "lv_Name";
            this.lv_Name.Size = new System.Drawing.Size(179, 295);
            this.lv_Name.TabIndex = 145;
            this.lv_Name.UseCompatibleStateImageBehavior = false;
            this.lv_Name.View = System.Windows.Forms.View.Details;
            // 
            // columnHeader1
            // 
            this.columnHeader1.Text = "名称";
            this.columnHeader1.Width = 174;
            // 
            // textBox1
            // 
            this.textBox1.Location = new System.Drawing.Point(52, 16);
            this.textBox1.Name = "textBox1";
            this.textBox1.Size = new System.Drawing.Size(142, 26);
            this.textBox1.TabIndex = 147;
            // 
            // label5
            // 
            this.label5.AutoSize = true;
            this.label5.Location = new System.Drawing.Point(6, 21);
            this.label5.Name = "label5";
            this.label5.Size = new System.Drawing.Size(40, 16);
            this.label5.TabIndex = 146;
            this.label5.Text = "名称";
            // 
            // groupBox1
            // 
            this.groupBox1.Controls.Add(this.txt_HarborPort);
            this.groupBox1.Controls.Add(this.label1);
            this.groupBox1.Controls.Add(this.label2);
            this.groupBox1.Controls.Add(this.txt_HarborIp);
            this.groupBox1.Controls.Add(this.bt_HarborLink);
            this.groupBox1.Location = new System.Drawing.Point(7, 8);
            this.groupBox1.Name = "groupBox1";
            this.groupBox1.Size = new System.Drawing.Size(382, 49);
            this.groupBox1.TabIndex = 148;
            this.groupBox1.TabStop = false;
            // 
            // groupBox2
            // 
            this.groupBox2.Controls.Add(this.txt_HarborSearch);
            this.groupBox2.Controls.Add(this.label3);
            this.groupBox2.Controls.Add(this.label4);
            this.groupBox2.Controls.Add(this.cb_HarborTask);
            this.groupBox2.Controls.Add(this.bt_HarborRun);
            this.groupBox2.Location = new System.Drawing.Point(4, 66);
            this.groupBox2.Name = "groupBox2";
            this.groupBox2.Size = new System.Drawing.Size(253, 81);
            this.groupBox2.TabIndex = 149;
            this.groupBox2.TabStop = false;
            // 
            // groupBox3
            // 
            this.groupBox3.Controls.Add(this.bt_HarborNew);
            this.groupBox3.Controls.Add(this.textBox1);
            this.groupBox3.Controls.Add(this.label5);
            this.groupBox3.Controls.Add(this.bt_HarborSave);
            this.groupBox3.Controls.Add(this.bt_HarborDel);
            this.groupBox3.Location = new System.Drawing.Point(273, 81);
            this.groupBox3.Name = "groupBox3";
            this.groupBox3.Size = new System.Drawing.Size(409, 54);
            this.groupBox3.TabIndex = 150;
            this.groupBox3.TabStop = false;
            // 
            // bt_HarborNew
            // 
            this.bt_HarborNew.Location = new System.Drawing.Point(279, 16);
            this.bt_HarborNew.Name = "bt_HarborNew";
            this.bt_HarborNew.Size = new System.Drawing.Size(50, 28);
            this.bt_HarborNew.TabIndex = 148;
            this.bt_HarborNew.Text = "新建";
            this.bt_HarborNew.UseVisualStyleBackColor = true;
            this.bt_HarborNew.Click += new System.EventHandler(this.bt_HarborNew_Click);
            // 
            // FmMain
            // 
            this.AutoScaleDimensions = new System.Drawing.SizeF(8F, 16F);
            this.AutoScaleMode = System.Windows.Forms.AutoScaleMode.Font;
            this.ClientSize = new System.Drawing.Size(927, 727);
            this.Controls.Add(this.splitContainer1);
            this.Controls.Add(this.menuStrip1);
            this.Font = new System.Drawing.Font("宋体", 12F);
            this.Icon = ((System.Drawing.Icon)(resources.GetObject("$this.Icon")));
            this.MainMenuStrip = this.menuStrip1;
            this.Margin = new System.Windows.Forms.Padding(4);
            this.MaximizeBox = false;
            this.Name = "FmMain";
            this.StartPosition = System.Windows.Forms.FormStartPosition.CenterScreen;
            this.Text = "命令";
            this.FormClosed += new System.Windows.Forms.FormClosedEventHandler(this.FmMain_FormClosed);
            this.splitContainer1.Panel1.ResumeLayout(false);
            this.splitContainer1.Panel2.ResumeLayout(false);
            this.splitContainer1.Panel2.PerformLayout();
            ((System.ComponentModel.ISupportInitialize)(this.splitContainer1)).EndInit();
            this.splitContainer1.ResumeLayout(false);
            this.tabControl1.ResumeLayout(false);
            this.tabPage1.ResumeLayout(false);
            this.tabPage1.PerformLayout();
            this.menuStrip1.ResumeLayout(false);
            this.menuStrip1.PerformLayout();
            this.groupBox1.ResumeLayout(false);
            this.groupBox1.PerformLayout();
            this.groupBox2.ResumeLayout(false);
            this.groupBox2.PerformLayout();
            this.groupBox3.ResumeLayout(false);
            this.groupBox3.PerformLayout();
            this.ResumeLayout(false);
            this.PerformLayout();

        }

        #endregion

        private System.Windows.Forms.SplitContainer splitContainer1;
        private System.Windows.Forms.TabControl tabControl1;
        private System.Windows.Forms.TabPage tabPage1;
        private System.Windows.Forms.TabPage tabPage2;
        private System.Windows.Forms.TextBox txt_Show;
        private System.Windows.Forms.TextBox txt_HarborPort;
        private System.Windows.Forms.TextBox txt_HarborIp;
        private System.Windows.Forms.Label label2;
        private System.Windows.Forms.Label label1;
        private System.Windows.Forms.Button bt_HarborLink;
        private System.Windows.Forms.Label label3;
        private System.Windows.Forms.ComboBox cb_HarborTask;
        private System.Windows.Forms.Label label4;
        private System.Windows.Forms.Button bt_HarborRun;
        private System.Windows.Forms.Button bt_HarborSave;
        private System.Windows.Forms.TextBox txt_HarborCMD;
        private System.Windows.Forms.TextBox txt_HarborSearch;
        private System.Windows.Forms.MenuStrip menuStrip1;
        private System.Windows.Forms.ToolStripMenuItem 操作ToolStripMenuItem;
        private System.Windows.Forms.ToolStripMenuItem 任务ToolStripMenuItem;
        private System.Windows.Forms.Button bt_HarborDel;
        private System.Windows.Forms.ListView lv_Name;
        private System.Windows.Forms.ColumnHeader columnHeader1;
        private System.Windows.Forms.TextBox textBox1;
        private System.Windows.Forms.Label label5;
        private System.Windows.Forms.GroupBox groupBox1;
        private System.Windows.Forms.GroupBox groupBox2;
        private System.Windows.Forms.GroupBox groupBox3;
        private System.Windows.Forms.Button bt_HarborNew;
    }
}

