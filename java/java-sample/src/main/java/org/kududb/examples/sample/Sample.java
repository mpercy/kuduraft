package org.kududb.examples.sample;

import org.kududb.ColumnSchema;
import org.kududb.Schema;
import org.kududb.Type;
import org.kududb.client.*;

import java.util.ArrayList;
import java.util.List;

public class Sample {

  private static final String KUDU_MASTER = System.getProperty(
      "kuduMaster", "quickstart.cloudera");

  public static void main(String[] args) {
    System.out.println("-----------------------------------------------");
    System.out.println("Will try to connect to Kudu master at " + KUDU_MASTER);
    System.out.println("Run with -DkuduMaster=myHost:port to override.");
    System.out.println("-----------------------------------------------");
    String tableName = "java_sample-" + System.currentTimeMillis();
    KuduClient client = new KuduClient.KuduClientBuilder(KUDU_MASTER).build();

    try {
      List<ColumnSchema> columns = new ArrayList(2);
      columns.add(new ColumnSchema.ColumnSchemaBuilder("key", Type.INT32)
          .key(true)
          .build());
      columns.add(new ColumnSchema.ColumnSchemaBuilder("value", Type.STRING)
          .build());
      Schema schema = new Schema(columns);
      client.createTable(tableName, schema);

      KuduTable table = client.openTable(tableName);
      KuduSession session = client.newSession();
      for (int i = 0; i < 3; i++) {
        Insert insert = table.newInsert();
        PartialRow row = insert.getRow();
        row.addInt(0, i);
        row.addString(1, "value " + i);
        session.apply(insert);
      }

      List<String> projectColumns = new ArrayList<>(1);
      projectColumns.add("value");
      KuduScanner scanner = client.newScannerBuilder(table)
          .setProjectedColumnNames(projectColumns)
          .build();
      while (scanner.hasMoreRows()) {
        RowResultIterator results = scanner.nextRows();
        while (results.hasNext()) {
          RowResult result = results.next();
          System.out.println(result.getString(0));
        }
      }
    } catch (Exception e) {
      e.printStackTrace();
    } finally {
      try {
        client.deleteTable(tableName);
      } catch (Exception e) {
        e.printStackTrace();
      } finally {
        try {
          client.shutdown();
        } catch (Exception e) {
          e.printStackTrace();
        }
      }
    }
  }
}

